#include <stic.h>

#include <sys/stat.h> /* chmod() */
#include <unistd.h> /* chdir() */

#include <stddef.h> /* NULL */
#include <stdlib.h> /* fclose() fopen() free() */
#include <string.h>
#include <wchar.h> /* wcsdup() */

#include "../../src/cfg/config.h"
#include "../../src/engine/abbrevs.h"
#include "../../src/engine/cmds.h"
#include "../../src/engine/completion.h"
#include "../../src/engine/functions.h"
#include "../../src/engine/options.h"
#include "../../src/modes/cmdline.h"
#include "../../src/utils/fs.h"
#include "../../src/utils/path.h"
#include "../../src/utils/str.h"
#include "../../src/bmarks.h"
#include "../../src/builtin_functions.h"
#include "../../src/cmd_core.h"

#if defined(__CYGWIN__) || defined(_WIN32)
#define SUFFIX ".exe"
#define SUFFIXW L".exe"
#else
#define SUFFIX ""
#define SUFFIXW L""
#endif

static void dummy_handler(OPT_OP op, optval_t val);
static void create_executable(const char file[]);
static int dquotes_allowed_in_paths(void);

static line_stats_t stats;
static char *saved_cwd;

SETUP()
{
	static int option_changed;
	optval_t def = { .str_val = "/tmp" };

	cfg.slow_fs_list = strdup("");

	init_builtin_functions();

	stats.line = wcsdup(L"set ");
	stats.index = wcslen(stats.line);
	stats.curs_pos = 0;
	stats.len = stats.index;
	stats.cmd_pos = -1;
	stats.complete_continue = 0;
	stats.history_search = 0;
	stats.line_buf = NULL;
	stats.complete = &complete_cmd;

	curr_view = &lwin;

	init_commands();

	execute_cmd("command bar a");
	execute_cmd("command baz b");
	execute_cmd("command foo c");

	init_options(&option_changed);
	add_option("fusehome", "fh", OPT_STR, OPT_GLOBAL, 0, NULL, &dummy_handler,
			def);
	add_option("path", "pt", OPT_STR, OPT_GLOBAL, 0, NULL, &dummy_handler, def);
	add_option("path", "pt", OPT_STR, OPT_LOCAL, 0, NULL, &dummy_handler, def);

	saved_cwd = save_cwd();
	assert_success(chdir(TEST_DATA_PATH "/existing-files"));
}

TEARDOWN()
{
	restore_cwd(saved_cwd);

	free(cfg.slow_fs_list);
	cfg.slow_fs_list = NULL;

	free(stats.line);
	reset_cmds();
	clear_options();

	function_reset_all();
}

static void
dummy_handler(OPT_OP op, optval_t val)
{
}

TEST(leave_spaces_at_begin)
{
	char *buf;

	vle_compl_reset();
	assert_int_equal(1, complete_cmd(" qui", NULL));
	buf = vle_compl_next();
	assert_string_equal("quit", buf);
	free(buf);
	buf = vle_compl_next();
	assert_string_equal("quit", buf);
	free(buf);
}

TEST(only_user)
{
	char *buf;

	vle_compl_reset();
	assert_int_equal(8, complete_cmd("command ", NULL));
	buf = vle_compl_next();
	assert_string_equal("bar", buf);
	free(buf);

	vle_compl_reset();
	assert_int_equal(9, complete_cmd(" command ", NULL));
	buf = vle_compl_next();
	assert_string_equal("bar", buf);
	free(buf);

	vle_compl_reset();
	assert_int_equal(10, complete_cmd("  command ", NULL));
	buf = vle_compl_next();
	assert_string_equal("bar", buf);
	free(buf);
}

TEST(test_set_completion)
{
	vle_compl_reset();
	assert_success(line_completion(&stats));
	assert_wstring_equal(L"set all", stats.line);
}

TEST(no_sdquoted_completion_does_nothing)
{
	free(stats.line);
	stats.line = wcsdup(L"command '");
	stats.len = wcslen(stats.line);
	stats.index = stats.len;

	vle_compl_reset();
	assert_success(line_completion(&stats));
	assert_wstring_equal(L"command '", stats.line);
}

static void
prepare_for_line_completion(const wchar_t str[])
{
	free(stats.line);
	stats.line = wcsdup(str);
	stats.len = wcslen(stats.line);
	stats.index = stats.len;
	stats.complete_continue = 0;

	vle_compl_reset();
}

TEST(spaces_escaping_leading)
{
	char *mb;

	assert_success(chdir("../spaces-in-names"));

	prepare_for_line_completion(L"touch \\ ");
	assert_success(line_completion(&stats));

	mb = to_multibyte(stats.line);
	assert_string_equal("touch \\ begins-with-space", mb);
	free(mb);
}

TEST(spaces_escaping_everywhere)
{
	char *mb;

	assert_success(chdir("../spaces-in-names"));

	prepare_for_line_completion(L"touch \\ s");
	assert_success(line_completion(&stats));

	mb = to_multibyte(stats.line);
	/* Whether trailing space is there depends on file system and OS. */
	if(access("\\ spaces\\ everywhere\\ ", F_OK) == 0)
	{
		assert_string_equal("touch \\ spaces\\ everywhere\\ ", mb);
	}
	/* Only one condition is true, but don't use else to make one of asserts fail
	 * if there are too files somehow. */
	if(access("\\ spaces\\ everywhere", F_OK) == 0)
	{
		assert_string_equal("touch \\ spaces\\ everywhere", mb);
	}
	free(mb);
}

TEST(spaces_escaping_trailing)
{
	char *mb;

	assert_success(chdir("../spaces-in-names"));

	prepare_for_line_completion(L"touch e");
	assert_success(line_completion(&stats));

	mb = to_multibyte(stats.line);
	/* Whether trailing space is there depends on file system and OS. */
	if(access("ends-with-space\\ ", F_OK) == 0)
	{
		assert_string_equal("touch ends-with-space\\ ", mb);
	}
	/* Only one condition is true, but don't use else to make one of asserts fail
	 * if there are too files somehow. */
	if(access("ends-with-space", F_OK) == 0)
	{
		assert_string_equal("touch ends-with-space", mb);
	}
	free(mb);
}

TEST(spaces_escaping_middle)
{
	char *mb;

	assert_success(chdir("../spaces-in-names"));

	prepare_for_line_completion(L"touch s");
	assert_success(line_completion(&stats));

	mb = to_multibyte(stats.line);
	assert_string_equal("touch spaces\\ in\\ the\\ middle", mb);
	free(mb);
}

TEST(squoted_completion)
{
	prepare_for_line_completion(L"touch '");
	assert_success(line_completion(&stats));
	assert_wstring_equal(L"touch 'a", stats.line);
}

TEST(squoted_completion_escaping)
{
	assert_success(chdir("../quotes-in-names"));

	prepare_for_line_completion(L"touch 's-quote");
	assert_success(line_completion(&stats));
	assert_wstring_equal(L"touch 's-quote-''-in-name", stats.line);
}

TEST(dquoted_completion)
{
	prepare_for_line_completion(L"touch 'b");
	assert_success(line_completion(&stats));
	assert_wstring_equal(L"touch 'b", stats.line);
}

TEST(dquoted_completion_escaping, IF(dquotes_allowed_in_paths))
{
	assert_success(chdir("../quotes-in-names"));

	prepare_for_line_completion(L"touch \"d-quote");
	assert_success(line_completion(&stats));
	assert_wstring_equal(L"touch \"d-quote-\\\"-in-name", stats.line);
}

TEST(last_match_is_properly_escaped)
{
	char *match;

	assert_success(chdir("../quotes-in-names"));

	prepare_for_line_completion(L"touch 's-quote-''-in");
	assert_success(line_completion(&stats));
	assert_wstring_equal(L"touch 's-quote-''-in-name", stats.line);

	match = vle_compl_next();
	assert_string_equal("s-quote-''-in-name-2", match);
	free(match);

	match = vle_compl_next();
	assert_string_equal("s-quote-''-in", match);
	free(match);
}

TEST(emark_cmd_escaping)
{
	char *match;

	prepare_for_line_completion(L"");
	assert_success(line_completion(&stats));
	assert_wstring_equal(L"!", stats.line);

	match = vle_compl_next();
	assert_string_equal("alink", match);
	free(match);
}

TEST(winrun_cmd_escaping)
{
	char *match;

	prepare_for_line_completion(L"winrun ");
	assert_success(line_completion(&stats));
	assert_wstring_equal(L"winrun $", stats.line);

	match = vle_compl_next();
	assert_string_equal("%", match);
	free(match);

	match = vle_compl_next();
	assert_string_equal(",", match);
	free(match);

	match = vle_compl_next();
	assert_string_equal(".", match);
	free(match);

	match = vle_compl_next();
	assert_string_equal("^", match);
	free(match);
}

TEST(help_cmd_escaping)
{
	cfg.use_vim_help = 1;

	prepare_for_line_completion(L"help vifm-");
	assert_success(line_completion(&stats));
	assert_wstring_equal(L"help vifm-!!", stats.line);
}

TEST(dirs_are_completed_with_trailing_slash)
{
	char *match;

	assert_success(chdir("../"));

	prepare_for_line_completion(L"cd r");
	assert_success(line_completion(&stats));
	assert_wstring_equal(L"cd read/", stats.line);

	match = vle_compl_next();
	assert_string_equal("rename/", match);
	free(match);

	match = vle_compl_next();
	assert_string_equal("r", match);
	free(match);

	match = vle_compl_next();
	assert_string_equal("read/", match);
	free(match);
}

TEST(function_name_completion)
{
	char *match;

	prepare_for_line_completion(L"echo e");
	assert_success(line_completion(&stats));
	assert_wstring_equal(L"echo executable(", stats.line);

	match = vle_compl_next();
	assert_string_equal("expand(", match);
	free(match);

	match = vle_compl_next();
	assert_string_equal("e", match);
	free(match);
}

TEST(percent_completion)
{
	char *match;

	/* One percent symbol. */

	prepare_for_line_completion(L"cd %");
	assert_success(line_completion(&stats));
	assert_wstring_equal(L"cd %%", stats.line);

	match = vle_compl_next();
	assert_string_equal("%%", match);
	free(match);

	match = vle_compl_next();
	assert_string_equal("%%", match);
	free(match);

	/* Two percent symbols. */

	prepare_for_line_completion(L"cd %%");
	assert_success(line_completion(&stats));
	assert_wstring_equal(L"cd %%", stats.line);

	match = vle_compl_next();
	assert_string_equal("%%", match);
	free(match);

	match = vle_compl_next();
	assert_string_equal("%%", match);
	free(match);

	/* Three percent symbols. */

	prepare_for_line_completion(L"cd %%%");
	assert_success(line_completion(&stats));
	assert_wstring_equal(L"cd %%%%", stats.line);

	match = vle_compl_next();
	assert_string_equal("%%%%", match);
	free(match);

	match = vle_compl_next();
	assert_string_equal("%%%%", match);
	free(match);
}

TEST(abbreviations)
{
	vle_abbr_reset();
	assert_success(vle_abbr_add(L"lhs", L"rhs"));

	prepare_for_line_completion(L"cabbrev l");
	assert_success(line_completion(&stats));
	assert_wstring_equal(L"cabbrev lhs", stats.line);

	prepare_for_line_completion(L"cnoreabbrev l");
	assert_success(line_completion(&stats));
	assert_wstring_equal(L"cnoreabbrev lhs", stats.line);

	prepare_for_line_completion(L"cunabbrev l");
	assert_success(line_completion(&stats));
	assert_wstring_equal(L"cunabbrev lhs", stats.line);

	prepare_for_line_completion(L"cabbrev l l");
	assert_success(line_completion(&stats));
	assert_wstring_equal(L"cabbrev l l", stats.line);

	vle_abbr_reset();
}

TEST(bang_abs_path_completion)
{
#if defined(_WIN32) && !defined(_WIN64)
#define WPRINTF_MBSTR L"S"
#else
#define WPRINTF_MBSTR L"s"
#endif

	wchar_t cmd[PATH_MAX];
	char cwd[PATH_MAX];

	restore_cwd(saved_cwd);
	saved_cwd = save_cwd();
	assert_success(chdir(SANDBOX_PATH));

	assert_true(get_cwd(cwd, sizeof(cwd)) == cwd);

	create_executable("exec-for-completion" SUFFIX);

	vifm_swprintf(cmd, ARRAY_LEN(cmd),
			L"!%" WPRINTF_MBSTR L"/exec-for-completion" SUFFIXW, cwd);

	prepare_for_line_completion(cmd);
	assert_success(line_completion(&stats));
	assert_wstring_equal(cmd, stats.line);

	assert_int_equal(2, vle_compl_get_count());

	assert_success(unlink("exec-for-completion" SUFFIX));

#undef WPRINTF_MBSTR
}

TEST(bmark_tags_are_completed)
{
	bmarks_clear();

	assert_success(exec_commands("bmark! fake/path1 tag1", &lwin, CIT_COMMAND));

	prepare_for_line_completion(L"bmark tag");
	assert_success(line_completion(&stats));
	assert_wstring_equal(L"bmark tag1", stats.line);

	prepare_for_line_completion(L"bmark! fake/path2 tag");
	assert_success(line_completion(&stats));
	assert_wstring_equal(L"bmark! fake/path2 tag1", stats.line);
}

TEST(bmark_path_is_completed)
{
	bmarks_clear();

	restore_cwd(saved_cwd);
	saved_cwd = save_cwd();
	assert_success(chdir(SANDBOX_PATH));
	create_executable("exec-for-completion" SUFFIX);

	prepare_for_line_completion(L"bmark! exec");
	assert_success(line_completion(&stats));
	assert_wstring_equal(L"bmark! exec-for-completion" SUFFIX, stats.line);

	assert_success(unlink("exec-for-completion" SUFFIX));
}

TEST(aucmd_events_are_completed)
{
	prepare_for_line_completion(L"autocmd ");
	assert_success(line_completion(&stats));
	assert_wstring_equal(L"autocmd DirEnter", stats.line);

	prepare_for_line_completion(L"autocmd Dir");
	assert_success(line_completion(&stats));
	assert_wstring_equal(L"autocmd DirEnter", stats.line);

	prepare_for_line_completion(L"autocmd! Dir");
	assert_success(line_completion(&stats));
	assert_wstring_equal(L"autocmd! DirEnter", stats.line);

	prepare_for_line_completion(L"autocmd DirEnter ");
	assert_success(line_completion(&stats));
	assert_wstring_equal(L"autocmd DirEnter ", stats.line);
}

TEST(prefixless_option_name_is_completed)
{
	prepare_for_line_completion(L"echo &");
	assert_success(line_completion(&stats));
	assert_wstring_equal(L"echo &fusehome", stats.line);
	assert_success(line_completion(&stats));
	assert_wstring_equal(L"echo &path", stats.line);
}

TEST(prefixed_global_option_name_is_completed)
{
	prepare_for_line_completion(L"echo &g:f");
	assert_success(line_completion(&stats));
	assert_wstring_equal(L"echo &g:fusehome", stats.line);
}

TEST(prefixed_local_option_name_is_completed)
{
	prepare_for_line_completion(L"echo &l:p");
	assert_success(line_completion(&stats));
	assert_wstring_equal(L"echo &l:path", stats.line);
}

TEST(autocmd_name_completion_is_case_insensitive)
{
	prepare_for_line_completion(L"autocmd dir");
	assert_success(line_completion(&stats));
	assert_wstring_equal(L"autocmd DirEnter", stats.line);
}

static void
create_executable(const char file[])
{
	FILE *const f = fopen(file, "w");
	if(f != NULL)
	{
		fclose(f);
	}
	assert_success(access(file, F_OK));
	chmod(file, 0755);
	assert_success(access(file, X_OK));
}

static int
dquotes_allowed_in_paths(void)
{
#if defined(__CYGWIN__) || defined(_WIN32)
	return 0;
#else
	return 1;
#endif
}

/* vim: set tabstop=2 softtabstop=2 shiftwidth=2 noexpandtab cinoptions-=(0 : */
/* vim: set cinoptions+=t0 filetype=c : */
