/* AUTO-GENERATED. */
#ifndef USER_BIN_TABLE_H
#define USER_BIN_TABLE_H
#include "hdrs/reboot.h"
#include "hdrs/tee.h"
#include "hdrs/neofetch.h"
#include "hdrs/cut.h"
#include "hdrs/true.h"
#include "hdrs/top.h"
#include "hdrs/env.h"
#include "hdrs/ls.h"
#include "hdrs/pwd.h"
#include "hdrs/id.h"
#include "hdrs/clear.h"
#include "hdrs/df.h"
#include "hdrs/lscpu.h"
#include "hdrs/renice.h"
#include "hdrs/color.h"
#include "hdrs/echo.h"
#include "hdrs/basename.h"
#include "hdrs/passwd.h"
#include "hdrs/tree.h"
#include "hdrs/tcc.h"
#include "hdrs/find.h"
#include "hdrs/edit.h"
#include "hdrs/useradd.h"
#include "hdrs/su.h"
#include "hdrs/yes.h"
#include "hdrs/free.h"
#include "hdrs/write.h"
#include "hdrs/sort.h"
#include "hdrs/ps.h"
#include "hdrs/hostname.h"
#include "hdrs/battery.h"
#include "hdrs/rmdir.h"
#include "hdrs/touch.h"
#include "hdrs/calc.h"
#include "hdrs/users.h"
#include "hdrs/mv.h"
#include "hdrs/head.h"
#include "hdrs/uniq.h"
#include "hdrs/logout.h"
#include "hdrs/grep.h"
#include "hdrs/false.h"
#include "hdrs/dirname.h"
#include "hdrs/groups.h"
#include "hdrs/which.h"
#include "hdrs/cp.h"
#include "hdrs/hexdump.h"
#include "hdrs/stat.h"
#include "hdrs/halt.h"
#include "hdrs/tail.h"
#include "hdrs/kill.h"
#include "hdrs/ringtest.h"
#include "hdrs/chown.h"
#include "hdrs/wc.h"
#include "hdrs/uname.h"
#include "hdrs/rm.h"
#include "hdrs/chmod.h"
#include "hdrs/whoami.h"
#include "hdrs/shutdown.h"
#include "hdrs/sync.h"
#include "hdrs/uptime.h"
#include "hdrs/date.h"
#include "hdrs/cat.h"
#include "hdrs/sysinfo.h"
#include "hdrs/seq.h"
#include "hdrs/mkdir.h"
#include "hdrs/sleep.h"
#include "hdrs/screeninfo.h"
typedef struct { const char *name; unsigned char *data; unsigned int *size_p; } user_bin_t;
static user_bin_t user_bins[] = {
    { "reboot", u_reboot, &u_reboot_len },
    { "tee", u_tee, &u_tee_len },
    { "neofetch", u_neofetch, &u_neofetch_len },
    { "cut", u_cut, &u_cut_len },
    { "true", u_true, &u_true_len },
    { "top", u_top, &u_top_len },
    { "env", u_env, &u_env_len },
    { "ls", u_ls, &u_ls_len },
    { "pwd", u_pwd, &u_pwd_len },
    { "id", u_id, &u_id_len },
    { "clear", u_clear, &u_clear_len },
    { "df", u_df, &u_df_len },
    { "lscpu", u_lscpu, &u_lscpu_len },
    { "renice", u_renice, &u_renice_len },
    { "color", u_color, &u_color_len },
    { "echo", u_echo, &u_echo_len },
    { "basename", u_basename, &u_basename_len },
    { "passwd", u_passwd, &u_passwd_len },
    { "tree", u_tree, &u_tree_len },
    { "tcc", u_tcc, &u_tcc_len },
    { "find", u_find, &u_find_len },
    { "edit", u_edit, &u_edit_len },
    { "useradd", u_useradd, &u_useradd_len },
    { "su", u_su, &u_su_len },
    { "yes", u_yes, &u_yes_len },
    { "free", u_free, &u_free_len },
    { "write", u_write, &u_write_len },
    { "sort", u_sort, &u_sort_len },
    { "ps", u_ps, &u_ps_len },
    { "hostname", u_hostname, &u_hostname_len },
    { "battery", u_battery, &u_battery_len },
    { "rmdir", u_rmdir, &u_rmdir_len },
    { "touch", u_touch, &u_touch_len },
    { "calc", u_calc, &u_calc_len },
    { "users", u_users, &u_users_len },
    { "mv", u_mv, &u_mv_len },
    { "head", u_head, &u_head_len },
    { "uniq", u_uniq, &u_uniq_len },
    { "logout", u_logout, &u_logout_len },
    { "grep", u_grep, &u_grep_len },
    { "false", u_false, &u_false_len },
    { "dirname", u_dirname, &u_dirname_len },
    { "groups", u_groups, &u_groups_len },
    { "which", u_which, &u_which_len },
    { "cp", u_cp, &u_cp_len },
    { "hexdump", u_hexdump, &u_hexdump_len },
    { "stat", u_stat, &u_stat_len },
    { "halt", u_halt, &u_halt_len },
    { "tail", u_tail, &u_tail_len },
    { "kill", u_kill, &u_kill_len },
    { "ringtest", u_ringtest, &u_ringtest_len },
    { "chown", u_chown, &u_chown_len },
    { "wc", u_wc, &u_wc_len },
    { "uname", u_uname, &u_uname_len },
    { "rm", u_rm, &u_rm_len },
    { "chmod", u_chmod, &u_chmod_len },
    { "whoami", u_whoami, &u_whoami_len },
    { "shutdown", u_shutdown, &u_shutdown_len },
    { "sync", u_sync, &u_sync_len },
    { "uptime", u_uptime, &u_uptime_len },
    { "date", u_date, &u_date_len },
    { "cat", u_cat, &u_cat_len },
    { "sysinfo", u_sysinfo, &u_sysinfo_len },
    { "seq", u_seq, &u_seq_len },
    { "mkdir", u_mkdir, &u_mkdir_len },
    { "sleep", u_sleep, &u_sleep_len },
    { "screeninfo", u_screeninfo, &u_screeninfo_len },
    { "cls", u_clear, &u_clear_len },
    { "nano", u_edit, &u_edit_len },
};
#define user_bins_count (sizeof(user_bins)/sizeof(user_bins[0]))
#endif
