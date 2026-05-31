#ifndef USER_USERPROG_H
#define USER_USERPROG_H

/* Ring-3 demonstration programs (run via usermode_run). */
void userprog_main(void);    /* friendly demo: prints via syscalls, exits   */
void userprog_badboy(void);  /* tries a privileged op -> should #GP fault   */

#endif /* USER_USERPROG_H */
