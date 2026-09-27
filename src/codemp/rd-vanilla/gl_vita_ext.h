/*
===========================================================================
Copyright (C) 2026, JK2VITA contributors

This file is part of JAVITA, a PS Vita port built on the OpenJK
source code.

OpenJK is free software; you can redistribute it and/or modify it
under the terms of the GNU General Public License version 2 as
published by the Free Software Foundation.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program; if not, see <http://www.gnu.org/licenses/>.
===========================================================================
*/

// NULL pointers and enums for the runtime-gated GL extensions GXM lacks (compiled arrays, ARB programs, NV combiners), plus no-op core shims
#ifndef JK2VITA_GL_VITA_EXT_H
#define JK2VITA_GL_VITA_EXT_H

#ifdef VITA

#include "gl_vita_types.h"

/* empty GL calling-convention macros */
#ifndef APIENTRY
#define APIENTRY
#endif
#ifndef GLAPIENTRY
#define GLAPIENTRY
#endif
#ifndef APIENTRYP
#define APIENTRYP APIENTRY *
#endif

#ifdef __cplusplus
extern "C" {
#endif

/* no-op core shims */
void glTexParameterfv(GLenum target, GLenum pname, const GLfloat *params);
void glDrawBuffer(GLenum mode);
void glArrayElement(GLint i);

/* extension enums at their GL registry values */
/* ARB vertex/fragment program */
#ifndef GL_VERTEX_PROGRAM_ARB
#define GL_VERTEX_PROGRAM_ARB            0x8620
#endif
#ifndef GL_FRAGMENT_PROGRAM_ARB
#define GL_FRAGMENT_PROGRAM_ARB          0x8804
#endif
#ifndef GL_PROGRAM_FORMAT_ASCII_ARB
#define GL_PROGRAM_FORMAT_ASCII_ARB      0x8875
#endif
/* NV register combiners */
#ifndef GL_REGISTER_COMBINERS_NV
#define GL_REGISTER_COMBINERS_NV         0x8522
#endif
#ifndef GL_COMBINER0_NV
#define GL_COMBINER0_NV                  0x8550
#endif
#ifndef GL_COMBINER1_NV
#define GL_COMBINER1_NV                  0x8551
#endif
#ifndef GL_NUM_GENERAL_COMBINERS_NV
#define GL_NUM_GENERAL_COMBINERS_NV      0x854E
#endif
#ifndef GL_CONSTANT_COLOR0_NV
#define GL_CONSTANT_COLOR0_NV            0x852A
#endif
#ifndef GL_DISCARD_NV
#define GL_DISCARD_NV                    0x8530
#endif
#ifndef GL_SPARE0_NV
#define GL_SPARE0_NV                     0x852E
#endif
#ifndef GL_SPARE1_NV
#define GL_SPARE1_NV                     0x852F
#endif
#ifndef GL_UNSIGNED_IDENTITY_NV
#define GL_UNSIGNED_IDENTITY_NV          0x8536
#endif
#ifndef GL_UNSIGNED_INVERT_NV
#define GL_UNSIGNED_INVERT_NV            0x8537
#endif
#ifndef GL_VARIABLE_A_NV
#define GL_VARIABLE_A_NV                 0x8523
#endif
#ifndef GL_VARIABLE_B_NV
#define GL_VARIABLE_B_NV                 0x8524
#endif
#ifndef GL_VARIABLE_C_NV
#define GL_VARIABLE_C_NV                 0x8525
#endif
#ifndef GL_VARIABLE_D_NV
#define GL_VARIABLE_D_NV                 0x8526
#endif

/* function-pointer types and NULL pointers for the unsupported extensions */

/* EXT_compiled_vertex_array */
typedef void (*PFNGLLOCKARRAYSEXTPROC)(GLint first, GLsizei count);
typedef void (*PFNGLUNLOCKARRAYSEXTPROC)(void);
extern PFNGLLOCKARRAYSEXTPROC   qglLockArraysEXT;
extern PFNGLUNLOCKARRAYSEXTPROC qglUnlockArraysEXT;

typedef void (*PFNGLTEXIMAGE3DPROC)(GLenum, GLint, GLint, GLsizei, GLsizei, GLsizei, GLint, GLenum, GLenum, const GLvoid *);
typedef void (*PFNGLTEXSUBIMAGE3DPROC)(GLenum, GLint, GLint, GLint, GLint, GLsizei, GLsizei, GLsizei, GLenum, GLenum, const GLvoid *);
extern PFNGLTEXIMAGE3DPROC    qglTexImage3D;
extern PFNGLTEXSUBIMAGE3DPROC qglTexSubImage3D;

/* ARB_vertex/fragment_program */
typedef void (*PFNGLPROGRAMSTRINGARBPROC)(GLenum, GLenum, GLsizei, const void *);
typedef void (*PFNGLBINDPROGRAMARBPROC)(GLenum, GLuint);
typedef void (*PFNGLDELETEPROGRAMSARBPROC)(GLsizei, const GLuint *);
typedef void (*PFNGLGENPROGRAMSARBPROC)(GLsizei, GLuint *);
typedef void (*PFNGLPROGRAMENVPARAMETER4DARBPROC)(GLenum, GLuint, GLdouble, GLdouble, GLdouble, GLdouble);
typedef void (*PFNGLPROGRAMENVPARAMETER4DVARBPROC)(GLenum, GLuint, const GLdouble *);
typedef void (*PFNGLPROGRAMENVPARAMETER4FARBPROC)(GLenum, GLuint, GLfloat, GLfloat, GLfloat, GLfloat);
typedef void (*PFNGLPROGRAMENVPARAMETER4FVARBPROC)(GLenum, GLuint, const GLfloat *);
typedef void (*PFNGLPROGRAMLOCALPARAMETER4DARBPROC)(GLenum, GLuint, GLdouble, GLdouble, GLdouble, GLdouble);
typedef void (*PFNGLPROGRAMLOCALPARAMETER4DVARBPROC)(GLenum, GLuint, const GLdouble *);
typedef void (*PFNGLPROGRAMLOCALPARAMETER4FARBPROC)(GLenum, GLuint, GLfloat, GLfloat, GLfloat, GLfloat);
typedef void (*PFNGLPROGRAMLOCALPARAMETER4FVARBPROC)(GLenum, GLuint, const GLfloat *);
typedef void (*PFNGLGETPROGRAMENVPARAMETERDVARBPROC)(GLenum, GLuint, GLdouble *);
typedef void (*PFNGLGETPROGRAMENVPARAMETERFVARBPROC)(GLenum, GLuint, GLfloat *);
typedef void (*PFNGLGETPROGRAMLOCALPARAMETERDVARBPROC)(GLenum, GLuint, GLdouble *);
typedef void (*PFNGLGETPROGRAMLOCALPARAMETERFVARBPROC)(GLenum, GLuint, GLfloat *);
typedef void (*PFNGLGETPROGRAMIVARBPROC)(GLenum, GLenum, GLint *);
typedef void (*PFNGLGETPROGRAMSTRINGARBPROC)(GLenum, GLenum, void *);
typedef GLboolean (*PFNGLISPROGRAMARBPROC)(GLuint);
extern PFNGLPROGRAMSTRINGARBPROC              qglProgramStringARB;
extern PFNGLBINDPROGRAMARBPROC                qglBindProgramARB;
extern PFNGLDELETEPROGRAMSARBPROC             qglDeleteProgramsARB;
extern PFNGLGENPROGRAMSARBPROC                qglGenProgramsARB;
extern PFNGLPROGRAMENVPARAMETER4DARBPROC      qglProgramEnvParameter4dARB;
extern PFNGLPROGRAMENVPARAMETER4DVARBPROC     qglProgramEnvParameter4dvARB;
extern PFNGLPROGRAMENVPARAMETER4FARBPROC      qglProgramEnvParameter4fARB;
extern PFNGLPROGRAMENVPARAMETER4FVARBPROC     qglProgramEnvParameter4fvARB;
extern PFNGLPROGRAMLOCALPARAMETER4DARBPROC    qglProgramLocalParameter4dARB;
extern PFNGLPROGRAMLOCALPARAMETER4DVARBPROC   qglProgramLocalParameter4dvARB;
extern PFNGLPROGRAMLOCALPARAMETER4FARBPROC    qglProgramLocalParameter4fARB;
extern PFNGLPROGRAMLOCALPARAMETER4FVARBPROC   qglProgramLocalParameter4fvARB;
extern PFNGLGETPROGRAMENVPARAMETERDVARBPROC   qglGetProgramEnvParameterdvARB;
extern PFNGLGETPROGRAMENVPARAMETERFVARBPROC   qglGetProgramEnvParameterfvARB;
extern PFNGLGETPROGRAMLOCALPARAMETERDVARBPROC qglGetProgramLocalParameterdvARB;
extern PFNGLGETPROGRAMLOCALPARAMETERFVARBPROC qglGetProgramLocalParameterfvARB;
extern PFNGLGETPROGRAMIVARBPROC               qglGetProgramivARB;
extern PFNGLGETPROGRAMSTRINGARBPROC           qglGetProgramStringARB;
extern PFNGLISPROGRAMARBPROC                  qglIsProgramARB;

/* NV_register_combiners */
typedef void (*PFNGLCOMBINERPARAMETERFVNVPROC)(GLenum, const GLfloat *);
typedef void (*PFNGLCOMBINERPARAMETERIVNVPROC)(GLenum, const GLint *);
typedef void (*PFNGLCOMBINERPARAMETERFNVPROC)(GLenum, GLfloat);
typedef void (*PFNGLCOMBINERPARAMETERINVPROC)(GLenum, GLint);
typedef void (*PFNGLCOMBINERINPUTNVPROC)(GLenum, GLenum, GLenum, GLenum, GLenum, GLenum);
typedef void (*PFNGLCOMBINEROUTPUTNVPROC)(GLenum, GLenum, GLenum, GLenum, GLenum, GLenum, GLenum, GLboolean, GLboolean, GLboolean);
typedef void (*PFNGLFINALCOMBINERINPUTNVPROC)(GLenum, GLenum, GLenum, GLenum);
typedef void (*PFNGLGETCOMBINERINPUTPARAMETERFVNVPROC)(GLenum, GLenum, GLenum, GLenum, GLfloat *);
typedef void (*PFNGLGETCOMBINERINPUTPARAMETERIVNVPROC)(GLenum, GLenum, GLenum, GLenum, GLint *);
typedef void (*PFNGLGETCOMBINEROUTPUTPARAMETERFVNVPROC)(GLenum, GLenum, GLenum, GLfloat *);
typedef void (*PFNGLGETCOMBINEROUTPUTPARAMETERIVNVPROC)(GLenum, GLenum, GLenum, GLint *);
typedef void (*PFNGLGETFINALCOMBINERINPUTPARAMETERFVNVPROC)(GLenum, GLenum, GLfloat *);
typedef void (*PFNGLGETFINALCOMBINERINPUTPARAMETERIVNVPROC)(GLenum, GLenum, GLint *);
extern PFNGLCOMBINERPARAMETERFVNVPROC                qglCombinerParameterfvNV;
extern PFNGLCOMBINERPARAMETERIVNVPROC                qglCombinerParameterivNV;
extern PFNGLCOMBINERPARAMETERFNVPROC                 qglCombinerParameterfNV;
extern PFNGLCOMBINERPARAMETERINVPROC                 qglCombinerParameteriNV;
extern PFNGLCOMBINERINPUTNVPROC                      qglCombinerInputNV;
extern PFNGLCOMBINEROUTPUTNVPROC                     qglCombinerOutputNV;
extern PFNGLFINALCOMBINERINPUTNVPROC                 qglFinalCombinerInputNV;
extern PFNGLGETCOMBINERINPUTPARAMETERFVNVPROC        qglGetCombinerInputParameterfvNV;
extern PFNGLGETCOMBINERINPUTPARAMETERIVNVPROC        qglGetCombinerInputParameterivNV;
extern PFNGLGETCOMBINEROUTPUTPARAMETERFVNVPROC       qglGetCombinerOutputParameterfvNV;
extern PFNGLGETCOMBINEROUTPUTPARAMETERIVNVPROC       qglGetCombinerOutputParameterivNV;
extern PFNGLGETFINALCOMBINERINPUTPARAMETERFVNVPROC   qglGetFinalCombinerInputParameterfvNV;
extern PFNGLGETFINALCOMBINERINPUTPARAMETERIVNVPROC   qglGetFinalCombinerInputParameterivNV;

#ifdef __cplusplus
}
#endif

#endif /* VITA */
#endif /* JK2VITA_GL_VITA_EXT_H */
