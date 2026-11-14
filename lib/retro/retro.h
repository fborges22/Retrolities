/* Retrolities common definitions, shared by every utility.
 *
 * The build system defines exactly one of RETRO_DOS, RETRO_WIN16,
 * RETRO_WIN9X, RETRO_WINNT or RETRO_WIN64, plus RETRO_BITS and either
 * RETRO_CONSOLE or RETRO_GUI.
 */
#ifndef RETRO_H
#define RETRO_H

#if defined(RETRO_WIN16) || defined(RETRO_WIN9X) || defined(RETRO_WINNT) || defined(RETRO_WIN64)
#define RETRO_WINDOWS 1
#endif
#if defined(RETRO_WIN9X) || defined(RETRO_WINNT) || defined(RETRO_WIN64)
#define RETRO_WIN32 1
#endif

#ifdef __cplusplus
extern "C" {
#endif

/* Name of the platform this binary was built for, e.g. "MS-DOS". */
const char *retro_target(void);

/* Describes the system the program is running on (e.g. "DOS 6.22") into
 * buf, which holds size bytes.  Returns buf. */
char *retro_host(char *buf, unsigned size);

#ifdef __cplusplus
}
#endif

#endif
