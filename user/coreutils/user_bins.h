/* AUTO-GENERATED por user/coreutils/build.sh — no editar. */
#ifndef USER_BIN_TABLE_H
#define USER_BIN_TABLE_H

#include "hdrs/reboot.h"
#include "hdrs/true.h"
#include "hdrs/ls.h"
#include "hdrs/pwd.h"
#include "hdrs/id.h"
#include "hdrs/clear.h"
#include "hdrs/echo.h"
#include "hdrs/yes.h"
#include "hdrs/hostname.h"
#include "hdrs/rmdir.h"
#include "hdrs/touch.h"
#include "hdrs/head.h"
#include "hdrs/grep.h"
#include "hdrs/false.h"
#include "hdrs/tail.h"
#include "hdrs/kill.h"
#include "hdrs/ringtest.h"
#include "hdrs/wc.h"
#include "hdrs/uname.h"
#include "hdrs/rm.h"
#include "hdrs/whoami.h"
#include "hdrs/shutdown.h"
#include "hdrs/sync.h"
#include "hdrs/uptime.h"
#include "hdrs/cat.h"
#include "hdrs/mkdir.h"
#include "hdrs/sleep.h"

typedef struct { const char *name; unsigned char *data; unsigned int *size_p; } user_bin_t;
static user_bin_t user_bins[] = {
    { "reboot", u_reboot, &u_reboot_len },
    { "true", u_true, &u_true_len },
    { "ls", u_ls, &u_ls_len },
    { "pwd", u_pwd, &u_pwd_len },
    { "id", u_id, &u_id_len },
    { "clear", u_clear, &u_clear_len },
    { "echo", u_echo, &u_echo_len },
    { "yes", u_yes, &u_yes_len },
    { "hostname", u_hostname, &u_hostname_len },
    { "rmdir", u_rmdir, &u_rmdir_len },
    { "touch", u_touch, &u_touch_len },
    { "head", u_head, &u_head_len },
    { "grep", u_grep, &u_grep_len },
    { "false", u_false, &u_false_len },
    { "tail", u_tail, &u_tail_len },
    { "kill", u_kill, &u_kill_len },
    { "ringtest", u_ringtest, &u_ringtest_len },
    { "wc", u_wc, &u_wc_len },
    { "uname", u_uname, &u_uname_len },
    { "rm", u_rm, &u_rm_len },
    { "whoami", u_whoami, &u_whoami_len },
    { "shutdown", u_shutdown, &u_shutdown_len },
    { "sync", u_sync, &u_sync_len },
    { "uptime", u_uptime, &u_uptime_len },
    { "cat", u_cat, &u_cat_len },
    { "mkdir", u_mkdir, &u_mkdir_len },
    { "sleep", u_sleep, &u_sleep_len },
};
#define user_bins_count (sizeof(user_bins)/sizeof(user_bins[0]))
#endif
