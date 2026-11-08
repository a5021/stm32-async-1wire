/**
 * @file ow_profile.h
 * @brief Optional compile-time hooks around the driver's real work.
 *
 * The public poll functions return immediately when no hardware operation has
 * completed. Measuring the whole poll entry point therefore measures the main
 * loop's polling frequency, not the driver's processing time. These hooks are
 * placed only around the branch that actually advances the state machine.
 *
 * `OW_PROFILE=1` makes the driver call the two functions; the application
 * provides them. With the macro unset the hooks compile to nothing.
 */

#ifndef OW_PROFILE_H
#define OW_PROFILE_H

#ifdef OW_PROFILE
void ow_profile_work_begin(void);
void ow_profile_work_end(void);
#define OW_PROFILE_WORK_BEGIN() ow_profile_work_begin()
#define OW_PROFILE_WORK_END() ow_profile_work_end()
#else
#define OW_PROFILE_WORK_BEGIN() ((void)0)
#define OW_PROFILE_WORK_END() ((void)0)
#endif

#endif /* OW_PROFILE_H */
