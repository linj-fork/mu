/*
** Copyright (C) 2020-2023 Dirk-Jan C. Binnema <djcb@djcbsoftware.nl>
**
** This program is free software; you can redistribute it and/or modify it
** under the terms of the GNU General Public License as published by the
** Free Software Foundation; either version 3, or (at your option) any
** later version.
**
** This program is distributed in the hope that it will be useful,
** but WITHOUT ANY WARRANTY; without even the implied warranty of
** MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
** GNU General Public License for more details.
**
** You should have received a copy of the GNU General Public License
** along with this program; if not, write to the Free Software Foundation,
** Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301, USA.
**
*/
#include "mu-scanner.hh"

#include "config.h"

#include <chrono>
#include <mutex>
#include <atomic>
#include <thread>
#include <cstring>

#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>

#include <glib.h>

#include "utils/mu-utils.hh"
#include "utils/mu-utils-file.hh"
#include "utils/mu-error.hh"

using namespace Mu;

using Mode = Scanner::Mode;

/*
 * dentry->d_ino, dentry->d_type may not be available
 */
struct dentry_t {
	dentry_t(const struct dirent *dentry):
#if HAVE_DIRENT_D_INO
		d_ino{dentry->d_ino},
#endif /*HAVE_DIRENT_D_INO*/

#if HAVE_DIRENT_D_TYPE
		d_type(dentry->d_type),
#endif /*HAVE_DIRENT_D_TYPE*/
		d_name{static_cast<const char*>(dentry->d_name)} {}
#if HAVE_DIRENT_D_INO
	ino_t		d_ino;
#endif /*HAVE_DIRENT_D_INO*/

#if HAVE_DIRENT_D_TYPE
	unsigned char	d_type;
#endif /*HAVE_DIRENT_D_TYPE*/

	std::string	d_name;
};

struct Scanner::Private {
	Private(const std::string& root_dir, Scanner::Handler handler, Mode mode):
		root_dir_{root_dir}, handler_{handler}, mode_{mode} {
		if (root_dir_.length() > PATH_MAX)
			throw Mu::Error{Error::Code::InvalidArgument,
				"path is too long"};
		if (!handler_)
			throw Mu::Error{Error::Code::InvalidArgument,
				"missing handler"};
	}
	~Private() { stop(); }

	Result<void> start();
	void stop();

	bool process_dentry(const std::string& path, const dentry_t& dentry,
			    bool is_maildir);
	bool process_dir(const std::string& path, bool is_maildir);

	int lazy_stat(const char *fullpath, struct stat *stat_buf,
		      const dentry_t& dentry);

	bool maildirs_only_mode() const { return mode_ == Mode::MaildirsOnly; }

	const std::string	root_dir_;
	const Scanner::Handler	handler_;
	Mode			mode_;
	std::atomic<bool>       running_{};
	std::mutex		lock_;
};

static bool
ignore_dentry(const dentry_t& dentry)
{
	const auto d_name{dentry.d_name.c_str()};

	/* dotdir? */
	if (d_name[0] == '\0' || (d_name[1] == '\0' && d_name[0] == '.') ||
	    (d_name[2] == '\0' && d_name[0] == '.' && d_name[1] == '.'))
		return true;

	if (g_strcmp0(d_name, "tmp") == 0)
			return true;

	if (d_name[0] == '.') {
		if (d_name[1] == '#') /* emacs? */
			return true;
		if (g_strcmp0(d_name + 1, "nnmaildir") == 0) /* gnus? */
			return true;
		if (g_strcmp0(d_name + 1, "notmuch") == 0) /* notmuch? */
			return true;
	}

	if (g_strcmp0(d_name, "hcache.db") == 0) /* mutt cache? */
		return true;

	return false; /* don't ignore */
}


/*
 * stat() if necessary (we'd like to avoid it), which we can if we only need the
 * file-type and we already have that from the dentry.
 */
int
Scanner::Private::lazy_stat(const char *path, struct stat *stat_buf, const dentry_t& dentry)
{
#if HAVE_DIRENT_D_TYPE
	if (maildirs_only_mode()) {
		switch (dentry.d_type) {
		case DT_REG:
			stat_buf->st_mode = S_IFREG;
			return 0;
		case DT_DIR:
			stat_buf->st_mode = S_IFDIR;
			return 0;
		default:
			/* LNK is inconclusive; we need a stat. */
			break;
		}
	}
#endif /*HAVE_DIRENT_D_TYPE*/

	int res = ::stat(path, stat_buf);
	if (res != 0)
		mu_warning("failed to stat {}: {}", path, g_strerror(errno));

	return res;
}


bool
Scanner::Private::process_dentry(const std::string& path, const dentry_t& dentry,
				 bool is_maildir)
{
	if (ignore_dentry(dentry))
		return true;

	auto call_handler=[&](auto&& path, auto&& statbuf, auto&& htype)->bool {
		return maildirs_only_mode() ? true : handler_(path, statbuf, htype);
	};

	const auto fullpath{join_paths(path, dentry.d_name)};
	struct stat statbuf{};
	if (lazy_stat(fullpath.c_str(), &statbuf, dentry) != 0)
		return false;

	if (maildirs_only_mode() && S_ISDIR(statbuf.st_mode) && dentry.d_name == "cur") {
		handler_(path/*without cur*/, {}, Scanner::HandleType::Maildir);
		return true; // found maildir; no need to recurse further.
	}

	if (S_ISDIR(statbuf.st_mode)) {
		const auto new_cur = dentry.d_name == "cur" || dentry.d_name == "new";
		const auto htype =
		    new_cur ?
			Scanner::HandleType::EnterNewCur :
			Scanner::HandleType::EnterDir;

		const auto res = call_handler(fullpath, &statbuf, htype);
		if (!res)
			return true; // skip

		process_dir(fullpath, new_cur);
		return call_handler(fullpath, &statbuf, Scanner::HandleType::LeaveDir);

	} else if (S_ISREG(statbuf.st_mode) && is_maildir)
		return call_handler(fullpath, &statbuf, Scanner::HandleType::File);

	mu_debug("skip {} (neither maildir-file nor directory)", fullpath);

	return true;
}

bool
Scanner::Private::process_dir(const std::string& path, bool is_maildir)
{
	if (!running_)
		return true; /* we're done */

	if (G_UNLIKELY(path.length() > PATH_MAX)) {
		// note: unlikely to hit this, one case would be a self-referential
		// symlink; that should be caught earlier, so this is just a backstop.
		mu_warning("path is too long: {}", path);
		return false;
	}

	const auto dir{::opendir(path.c_str())};
	if (G_UNLIKELY(!dir)) {
		mu_warning("failed to scan dir {}: {}", path, g_strerror(errno));
		return false;
	}

	std::vector<dentry_t> dir_entries;
	while (running_) {
		errno = 0;
		if (const auto& dentry{::readdir(dir)}; dentry) {
#if HAVE_DIRENT_D_TYPE /* opttimization: filter out non-dirs early */
			if (maildirs_only_mode() &&
			    dentry->d_type != DT_DIR && dentry->d_type != DT_LNK)
				continue;
#endif /*HAVE_DIRENT_D_TYPE*/
			dir_entries.emplace_back(dentry);
			continue;
		} else if (errno != 0) {
			mu_warning("failed to read {}: {}", path, g_strerror(errno));
			continue;
		}

		break;
	}
	::closedir(dir);

#if HAVE_DIRENT_D_INO
	// sort by i-node; much faster on rotational (HDDs) devices and on SSDs
	// sort is quick enough to not matter much
	std::sort(dir_entries.begin(), dir_entries.end(),
		  [](auto&& d1, auto&& d2){ return d1.d_ino < d2.d_ino; });
#endif /*HAVEN_DIRENT_D_INO*/

	// now process...
	for (auto&& dentry: dir_entries)
		process_dentry(path, dentry, is_maildir);

	return true;
}

Result<void>
Scanner::Private::start()
{
	const auto mode{F_OK | R_OK};
	if (G_UNLIKELY(::access(root_dir_.c_str(), mode) != 0))
		return Err(Error::Code::File,
			   "'{}' is not readable: {}", root_dir_,
			   g_strerror(errno));

	struct stat statbuf {};
	if (G_UNLIKELY(::stat(root_dir_.c_str(), &statbuf) != 0))
		return Err(Error::Code::File,
			   "'{}' is not stat'able: {}",
			   root_dir_, g_strerror(errno));

	if (G_UNLIKELY(!S_ISDIR(statbuf.st_mode)))
		return Err(Error::Code::File,
			   "'{}' is not a directory", root_dir_);

	running_ = true;
	mu_debug("starting scan @ {}", root_dir_);

	auto basename{to_string_gchar(g_path_get_basename(root_dir_.c_str()))};
	const auto is_maildir = basename == "cur" || basename == "new";

	const auto start{std::chrono::steady_clock::now()};
	process_dir(root_dir_, is_maildir);
	const auto elapsed = std::chrono::steady_clock::now() - start;
	mu_debug("finished scan of {} in {} ms", root_dir_, to_ms(elapsed));
	running_ = false;

	return Ok();
}

void
Scanner::Private::stop()
{
	if (running_) {
		mu_debug("stopping scan");
		running_ = false;
	}
}

Scanner::Scanner(const std::string& root_dir, Scanner::Handler handler, Mode flavor)
    : priv_{std::make_unique<Private>(root_dir, handler, flavor)}
{
}

Scanner::~Scanner() = default;

Result<void>
Scanner::start()
{
	if (priv_->running_)
		return Ok(); // nothing to do

	auto res  = priv_->start(); /* blocks */
	priv_->running_ = false;

	return res;
}

void
Scanner::stop()
{
	std::lock_guard l(priv_->lock_);
	priv_->stop();
}

bool
Scanner::is_running() const
{
	return priv_->running_;
}


#if BUILD_TESTS
#include "mu-test-utils.hh"

static void
test_scan_maildir()
{
	allow_warnings();

	size_t count{};
	Scanner scanner{
		MU_TESTMAILDIR,
		[&](const std::string& fullpath, const struct stat* statbuf, auto&& htype) -> bool {
			mu_debug("{} {}", fullpath, statbuf->st_size);
			++count;
			return true;
		}};
	g_assert_true(scanner.start());

	while (scanner.is_running()) { g_usleep(1000); }

	// very rudimentary test...
	g_assert_cmpuint(count,==,23);
}

int
main(int argc, char* argv[])
try {
	g_test_init(&argc, &argv, NULL);

	g_test_add_func("/index/scanner/scan-maildir", test_scan_maildir);

	return g_test_run();

} catch (const std::runtime_error& re) {
	mu_printerrln("caught runtime error: {}", re.what());
	return 1;
} catch (...) {
	mu_printerrln("caught exception");
	return 1;
}
#endif /*BUILD_TESTS*/

#if BUILD_LIST_MAILDIRS

static bool
on_path(const std::string& path, struct stat* statbuf, Scanner::HandleType htype)
{
	mu_println("{}", path);
	return true;
}

int
main (int argc, char *argv[])
{
	if (argc < 2) {
		mu_printerrln("expected: path to maildir");
		return 1;
	}

	Scanner scanner{argv[1], on_path, Mode::MaildirsOnly};

	scanner.start();

	return 0;
}
#endif /*BUILD_LIST_MAILDIRS*/
