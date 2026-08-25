/* gl-fwd.c -- OpenGL/GLX forwarding shim for hosts whose GL is classic Mesa.
 *
 * The AppImage bundles glibc libglvnd, whose libGL.so.1 is a dispatcher that
 * dlopen's a vendor library (libGLX_mesa.so.0). That vendor library does not
 * exist on any host whose Mesa is built without glvnd -- every musl distro
 * (Alpine) AND every pre-glvnd glibc distro (Ubuntu 14.04, Debian 8). On those
 * hosts glxgears fails with "couldn't get an RGB, Double-buffered visual".
 *
 * This shim, preloaded AFTER foreign-dlopen.so and built with the SONAME
 * libGL.so.1, makes ld.so bind an app's DT_NEEDED libGL.so.1 to the shim
 * instead of the bundled dispatcher. It dlopen's the HOST's libGL.so.1 through
 * foreign-dlopen.so (which strips version tags, drops a musl libc dependency
 * and bridges the libc gap), then forwards the GL/GLX entry points. The heavy
 * lifting (musl->glibc bridge, version traps, ___environ rename) is exactly
 * what already makes vkcube work.
 *
 * It always forwards: on a glvnd host the host libGL.so.1 is itself a glvnd
 * dispatcher, and forwarding through it reaches the host libGLX_mesa.so.0 the
 * same way the bundled dispatcher would. So the shim is safe to preload
 * unconditionally.
 *
 * NOT a full solution: only the 33 symbols glxgears imports are forwarded,
 * and each wrapper degrades to the documented failure value (NULL visual,
 * False, 0) when the host libGL is absent. A production version would
 * generate the full GL/GLX/EGL/GLES wrapper set from the OpenGL registry.
 */

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

/* ---- minimal, spec-stable GL / GLX types (pointers-only for X11) ---- */
typedef unsigned int  GLenum;
typedef unsigned int  GLbitfield;
typedef int           GLint;
typedef int           GLsizei;
typedef unsigned int  GLuint;
typedef float         GLfloat;
typedef double        GLdouble;
typedef unsigned char GLubyte;

typedef struct _XDisplay    Display;
typedef struct _XVisualInfo XVisualInfo;
typedef int                 Bool;
typedef unsigned long       XID;

typedef struct __GLXcontextRec *GLXContext;
typedef XID GLXDrawable;
typedef void (*__GLXextFuncPtr)(void);

#define VISIBLE __attribute__((visibility("default")))

/* ---- GLX prototypes (GLX 1.0-1.3, what glxgears links) ---- */
VISIBLE XVisualInfo *glXChooseVisual(Display *dpy, int screen, int *attribList);
VISIBLE GLXContext   glXCreateContext(Display *dpy, XVisualInfo *vis, GLXContext shareList, Bool direct);
VISIBLE void         glXDestroyContext(Display *dpy, GLXContext ctx);
VISIBLE Bool         glXMakeCurrent(Display *dpy, GLXDrawable drawable, GLXContext ctx);
VISIBLE void         glXSwapBuffers(Display *dpy, GLXDrawable drawable);
VISIBLE const char  *glXQueryExtensionsString(Display *dpy, int screen);
VISIBLE void         glXQueryDrawable(Display *dpy, GLXDrawable drawable, int attribute, unsigned int *value);
VISIBLE __GLXextFuncPtr glXGetProcAddressARB(const GLubyte *procName);

/* ---- GL prototypes (classic 1.x, what glxgears links) ---- */
VISIBLE void glBegin(GLenum mode);
VISIBLE void glCallList(GLuint list);
VISIBLE void glClear(GLbitfield mask);
VISIBLE void glDeleteLists(GLuint list, GLsizei range);
VISIBLE void glDrawBuffer(GLenum mode);
VISIBLE void glEnable(GLenum cap);
VISIBLE void glEnd(void);
VISIBLE void glEndList(void);
VISIBLE void glFrustum(GLdouble left, GLdouble right, GLdouble bottom, GLdouble top, GLdouble zNear, GLdouble zFar);
VISIBLE GLuint glGenLists(GLsizei range);
VISIBLE const GLubyte *glGetString(GLenum name);
VISIBLE void glLightfv(GLenum light, GLenum pname, const GLfloat *params);
VISIBLE void glLoadIdentity(void);
VISIBLE void glMaterialfv(GLenum face, GLenum pname, const GLfloat *params);
VISIBLE void glMatrixMode(GLenum mode);
VISIBLE void glNewList(GLuint list, GLenum mode);
VISIBLE void glNormal3f(GLfloat nx, GLfloat ny, GLfloat nz);
VISIBLE void glPopMatrix(void);
VISIBLE void glPushMatrix(void);
VISIBLE void glRotatef(GLfloat angle, GLfloat x, GLfloat y, GLfloat z);
VISIBLE void glShadeModel(GLenum mode);
VISIBLE void glTranslated(GLdouble x, GLdouble y, GLdouble z);
VISIBLE void glTranslatef(GLfloat x, GLfloat y, GLfloat z);
VISIBLE void glVertex3f(GLfloat x, GLfloat y, GLfloat z);
VISIBLE void glViewport(GLint x, GLint y, GLsizei width, GLsizei height);

/* ---- resolved function pointers into the host libGL ---- */
static void *host_gl;

#define PTR(name) static __typeof__(name) *name##_p

PTR(glXChooseVisual);
PTR(glXCreateContext);
PTR(glXDestroyContext);
PTR(glXMakeCurrent);
PTR(glXSwapBuffers);
PTR(glXQueryExtensionsString);
PTR(glXQueryDrawable);
PTR(glXGetProcAddressARB);
PTR(glBegin);
PTR(glCallList);
PTR(glClear);
PTR(glDeleteLists);
PTR(glDrawBuffer);
PTR(glEnable);
PTR(glEnd);
PTR(glEndList);
PTR(glFrustum);
PTR(glGenLists);
PTR(glGetString);
PTR(glLightfv);
PTR(glLoadIdentity);
PTR(glMaterialfv);
PTR(glMatrixMode);
PTR(glNewList);
PTR(glNormal3f);
PTR(glPopMatrix);
PTR(glPushMatrix);
PTR(glRotatef);
PTR(glShadeModel);
PTR(glTranslated);
PTR(glTranslatef);
PTR(glVertex3f);
PTR(glViewport);

/* ---- wrappers ----
 * Every wrapper is NULL-safe: when the host libGL could not be loaded the
 * pointers stay NULL and the calls degrade to the documented failure value
 * (NULL visual/context, False, 0, no-op) instead of jumping through NULL.
 * glXChooseVisual returning NULL is what makes glxgears print "couldn't get
 * an RGB, Double-buffered visual" and exit cleanly, exactly as glvnd does
 * when it finds no vendor. */
VISIBLE XVisualInfo *glXChooseVisual(Display *dpy, int screen, int *attribList) {
	return glXChooseVisual_p ? glXChooseVisual_p(dpy, screen, attribList) : NULL;
}
VISIBLE GLXContext glXCreateContext(Display *dpy, XVisualInfo *vis, GLXContext shareList, Bool direct) {
	return glXCreateContext_p ? glXCreateContext_p(dpy, vis, shareList, direct) : NULL;
}
VISIBLE void glXDestroyContext(Display *dpy, GLXContext ctx) {
	if (glXDestroyContext_p) glXDestroyContext_p(dpy, ctx);
}
VISIBLE Bool glXMakeCurrent(Display *dpy, GLXDrawable drawable, GLXContext ctx) {
	return glXMakeCurrent_p ? glXMakeCurrent_p(dpy, drawable, ctx) : 0;
}
VISIBLE void glXSwapBuffers(Display *dpy, GLXDrawable drawable) {
	if (glXSwapBuffers_p) glXSwapBuffers_p(dpy, drawable);
}
VISIBLE const char *glXQueryExtensionsString(Display *dpy, int screen) {
	return glXQueryExtensionsString_p ? glXQueryExtensionsString_p(dpy, screen) : NULL;
}
VISIBLE void glXQueryDrawable(Display *dpy, GLXDrawable drawable, int attribute, unsigned int *value) {
	if (glXQueryDrawable_p) glXQueryDrawable_p(dpy, drawable, attribute, value);
}
VISIBLE __GLXextFuncPtr glXGetProcAddressARB(const GLubyte *procName) {
	return glXGetProcAddressARB_p ? glXGetProcAddressARB_p(procName) : NULL;
}
VISIBLE void glBegin(GLenum mode) { if (glBegin_p) glBegin_p(mode); }
VISIBLE void glCallList(GLuint list) { if (glCallList_p) glCallList_p(list); }
VISIBLE void glClear(GLbitfield mask) { if (glClear_p) glClear_p(mask); }
VISIBLE void glDeleteLists(GLuint list, GLsizei range) { if (glDeleteLists_p) glDeleteLists_p(list, range); }
VISIBLE void glDrawBuffer(GLenum mode) { if (glDrawBuffer_p) glDrawBuffer_p(mode); }
VISIBLE void glEnable(GLenum cap) { if (glEnable_p) glEnable_p(cap); }
VISIBLE void glEnd(void) { if (glEnd_p) glEnd_p(); }
VISIBLE void glEndList(void) { if (glEndList_p) glEndList_p(); }
VISIBLE void glFrustum(GLdouble l, GLdouble r, GLdouble b, GLdouble t, GLdouble n, GLdouble f) {
	if (glFrustum_p) glFrustum_p(l, r, b, t, n, f);
}
VISIBLE GLuint glGenLists(GLsizei range) { return glGenLists_p ? glGenLists_p(range) : 0; }
VISIBLE const GLubyte *glGetString(GLenum name) { return glGetString_p ? glGetString_p(name) : NULL; }
VISIBLE void glLightfv(GLenum light, GLenum pname, const GLfloat *params) {
	if (glLightfv_p) glLightfv_p(light, pname, params);
}
VISIBLE void glLoadIdentity(void) { if (glLoadIdentity_p) glLoadIdentity_p(); }
VISIBLE void glMaterialfv(GLenum face, GLenum pname, const GLfloat *params) {
	if (glMaterialfv_p) glMaterialfv_p(face, pname, params);
}
VISIBLE void glMatrixMode(GLenum mode) { if (glMatrixMode_p) glMatrixMode_p(mode); }
VISIBLE void glNewList(GLuint list, GLenum mode) { if (glNewList_p) glNewList_p(list, mode); }
VISIBLE void glNormal3f(GLfloat nx, GLfloat ny, GLfloat nz) { if (glNormal3f_p) glNormal3f_p(nx, ny, nz); }
VISIBLE void glPopMatrix(void) { if (glPopMatrix_p) glPopMatrix_p(); }
VISIBLE void glPushMatrix(void) { if (glPushMatrix_p) glPushMatrix_p(); }
VISIBLE void glRotatef(GLfloat a, GLfloat x, GLfloat y, GLfloat z) { if (glRotatef_p) glRotatef_p(a, x, y, z); }
VISIBLE void glShadeModel(GLenum mode) { if (glShadeModel_p) glShadeModel_p(mode); }
VISIBLE void glTranslated(GLdouble x, GLdouble y, GLdouble z) { if (glTranslated_p) glTranslated_p(x, y, z); }
VISIBLE void glTranslatef(GLfloat x, GLfloat y, GLfloat z) { if (glTranslatef_p) glTranslatef_p(x, y, z); }
VISIBLE void glVertex3f(GLfloat x, GLfloat y, GLfloat z) { if (glVertex3f_p) glVertex3f_p(x, y, z); }
VISIBLE void glViewport(GLint x, GLint y, GLsizei w, GLsizei h) { if (glViewport_p) glViewport_p(x, y, w, h); }

/* ---- load the host classic libGL through foreign-dlopen ---- */
#define RESOLVE(name) do { \
	name##_p = (__typeof__(name##_p)) dlsym(host_gl, #name); \
	if (!name##_p) fprintf(stderr, "[gl-fwd] host libGL lacks %s\n", #name); \
} while (0)

static const char *host_candidates[] = {
	"/usr/lib/libGL.so.1",
	"/usr/lib/mesa/libGL.so.1",
	"/usr/lib64/libGL.so.1",
	"/usr/lib64/mesa/libGL.so.1",
	"/usr/lib/x86_64-linux-gnu/libGL.so.1",
	"/usr/lib/x86_64-linux-gnu/mesa/libGL.so.1",
	"/lib/libGL.so.1",
	"/lib64/libGL.so.1",
	NULL
};

/* The shim is always the app's libGL.so.1: its SONAME matches glxgears'
 * DT_NEEDED, so ld.so binds the app to the shim and never loads the bundled
 * glvnd dispatcher. It must therefore always forward, to whatever libGL.so.1
 * the host ships:
 *
 *   - classic Mesa: every musl distro, plus pre-glvnd glibc distros such as
 *     Ubuntu 14.04 and Debian 8. No glvnd vendor lib exists there, which is
 *     the exact failure this shim fixes.
 *   - glvnd: modern glibc distros. Forwarding reaches the host's glvnd
 *     dispatcher, which loads the host's libGLX_mesa.so.0 the same way the
 *     bundled dispatcher would have.
 *
 * host_has_glvnd() only selects a debug message, not behaviour. */
static int host_has_glvnd(void) {
	const char *markers[] = {
		"/usr/lib/libGLX.so.0",
		"/usr/lib64/libGLX.so.0",
		"/usr/lib/x86_64-linux-gnu/libGLX.so.0",
		"/lib/libGLX.so.0",
		"/lib64/libGLX.so.0",
		NULL
	};
	const char **m;
	for (m = markers; *m; m++)
		if (access(*m, F_OK) == 0)
			return 1;
	return 0;
}

__attribute__((constructor))
static void glfwd_init(void) {
	const char **c;
	for (c = host_candidates; *c; c++) {
		host_gl = dlopen(*c, RTLD_LAZY | RTLD_LOCAL);
		if (host_gl)
			break;
	}
	if (!host_gl) {
		fprintf(stderr, "[gl-fwd] could not load any host libGL.so.1\n");
		return;
	}
	fprintf(stderr, "[gl-fwd] using host libGL: %s (%s)\n", *c,
	        host_has_glvnd() ? "glvnd" : "classic");

	RESOLVE(glXChooseVisual);
	RESOLVE(glXCreateContext);
	RESOLVE(glXDestroyContext);
	RESOLVE(glXMakeCurrent);
	RESOLVE(glXSwapBuffers);
	RESOLVE(glXQueryExtensionsString);
	RESOLVE(glXQueryDrawable);
	RESOLVE(glXGetProcAddressARB);
	RESOLVE(glBegin);
	RESOLVE(glCallList);
	RESOLVE(glClear);
	RESOLVE(glDeleteLists);
	RESOLVE(glDrawBuffer);
	RESOLVE(glEnable);
	RESOLVE(glEnd);
	RESOLVE(glEndList);
	RESOLVE(glFrustum);
	RESOLVE(glGenLists);
	RESOLVE(glGetString);
	RESOLVE(glLightfv);
	RESOLVE(glLoadIdentity);
	RESOLVE(glMaterialfv);
	RESOLVE(glMatrixMode);
	RESOLVE(glNewList);
	RESOLVE(glNormal3f);
	RESOLVE(glPopMatrix);
	RESOLVE(glPushMatrix);
	RESOLVE(glRotatef);
	RESOLVE(glShadeModel);
	RESOLVE(glTranslated);
	RESOLVE(glTranslatef);
	RESOLVE(glVertex3f);
	RESOLVE(glViewport);
}
