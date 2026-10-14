/*
 * Copyright (C) 2026 The LineageOS Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

/* Forward GLES2 entry points to libMali.so; glesApi.c wraps the precision query. */

.text
.align 2
.globl glActiveTexture
.type glActiveTexture, %function
glActiveTexture:
    b shim_glActiveTexture
.size glActiveTexture, .-glActiveTexture

.globl glAttachShader
.type glAttachShader, %function
glAttachShader:
    b shim_glAttachShader
.size glAttachShader, .-glAttachShader

.globl glBindAttribLocation
.type glBindAttribLocation, %function
glBindAttribLocation:
    b shim_glBindAttribLocation
.size glBindAttribLocation, .-glBindAttribLocation

.globl glBindBuffer
.type glBindBuffer, %function
glBindBuffer:
    b shim_glBindBuffer
.size glBindBuffer, .-glBindBuffer

.globl glBindFramebuffer
.type glBindFramebuffer, %function
glBindFramebuffer:
    b shim_glBindFramebuffer
.size glBindFramebuffer, .-glBindFramebuffer

.globl glBindRenderbuffer
.type glBindRenderbuffer, %function
glBindRenderbuffer:
    b shim_glBindRenderbuffer
.size glBindRenderbuffer, .-glBindRenderbuffer

.globl glBindTexture
.type glBindTexture, %function
glBindTexture:
    b shim_glBindTexture
.size glBindTexture, .-glBindTexture

.globl glBlendColor
.type glBlendColor, %function
glBlendColor:
    b shim_glBlendColor
.size glBlendColor, .-glBlendColor

.globl glBlendEquation
.type glBlendEquation, %function
glBlendEquation:
    b shim_glBlendEquation
.size glBlendEquation, .-glBlendEquation

.globl glBlendEquationSeparate
.type glBlendEquationSeparate, %function
glBlendEquationSeparate:
    b shim_glBlendEquationSeparate
.size glBlendEquationSeparate, .-glBlendEquationSeparate

.globl glBlendFunc
.type glBlendFunc, %function
glBlendFunc:
    b shim_glBlendFunc
.size glBlendFunc, .-glBlendFunc

.globl glBlendFuncSeparate
.type glBlendFuncSeparate, %function
glBlendFuncSeparate:
    b shim_glBlendFuncSeparate
.size glBlendFuncSeparate, .-glBlendFuncSeparate

.globl glBufferData
.type glBufferData, %function
glBufferData:
    b shim_glBufferData
.size glBufferData, .-glBufferData

.globl glBufferSubData
.type glBufferSubData, %function
glBufferSubData:
    b shim_glBufferSubData
.size glBufferSubData, .-glBufferSubData

.globl glCheckFramebufferStatus
.type glCheckFramebufferStatus, %function
glCheckFramebufferStatus:
    b shim_glCheckFramebufferStatus
.size glCheckFramebufferStatus, .-glCheckFramebufferStatus

.globl glClear
.type glClear, %function
glClear:
    b shim_glClear
.size glClear, .-glClear

.globl glClearColor
.type glClearColor, %function
glClearColor:
    b shim_glClearColor
.size glClearColor, .-glClearColor

.globl glClearDepthf
.type glClearDepthf, %function
glClearDepthf:
    b shim_glClearDepthf
.size glClearDepthf, .-glClearDepthf

.globl glClearStencil
.type glClearStencil, %function
glClearStencil:
    b shim_glClearStencil
.size glClearStencil, .-glClearStencil

.globl glColorMask
.type glColorMask, %function
glColorMask:
    b shim_glColorMask
.size glColorMask, .-glColorMask

.globl glCompileShader
.type glCompileShader, %function
glCompileShader:
    b shim_glCompileShader
.size glCompileShader, .-glCompileShader

.globl glCompressedTexImage2D
.type glCompressedTexImage2D, %function
glCompressedTexImage2D:
    b shim_glCompressedTexImage2D
.size glCompressedTexImage2D, .-glCompressedTexImage2D

.globl glCompressedTexSubImage2D
.type glCompressedTexSubImage2D, %function
glCompressedTexSubImage2D:
    b shim_glCompressedTexSubImage2D
.size glCompressedTexSubImage2D, .-glCompressedTexSubImage2D

.globl glCopyTexImage2D
.type glCopyTexImage2D, %function
glCopyTexImage2D:
    b shim_glCopyTexImage2D
.size glCopyTexImage2D, .-glCopyTexImage2D

.globl glCopyTexSubImage2D
.type glCopyTexSubImage2D, %function
glCopyTexSubImage2D:
    b shim_glCopyTexSubImage2D
.size glCopyTexSubImage2D, .-glCopyTexSubImage2D

.globl glCreateProgram
.type glCreateProgram, %function
glCreateProgram:
    b shim_glCreateProgram
.size glCreateProgram, .-glCreateProgram

.globl glCreateShader
.type glCreateShader, %function
glCreateShader:
    b shim_glCreateShader
.size glCreateShader, .-glCreateShader

.globl glCullFace
.type glCullFace, %function
glCullFace:
    b shim_glCullFace
.size glCullFace, .-glCullFace

.globl glDeleteBuffers
.type glDeleteBuffers, %function
glDeleteBuffers:
    b shim_glDeleteBuffers
.size glDeleteBuffers, .-glDeleteBuffers

.globl glDeleteFramebuffers
.type glDeleteFramebuffers, %function
glDeleteFramebuffers:
    b shim_glDeleteFramebuffers
.size glDeleteFramebuffers, .-glDeleteFramebuffers

.globl glDeleteProgram
.type glDeleteProgram, %function
glDeleteProgram:
    b shim_glDeleteProgram
.size glDeleteProgram, .-glDeleteProgram

.globl glDeleteRenderbuffers
.type glDeleteRenderbuffers, %function
glDeleteRenderbuffers:
    b shim_glDeleteRenderbuffers
.size glDeleteRenderbuffers, .-glDeleteRenderbuffers

.globl glDeleteShader
.type glDeleteShader, %function
glDeleteShader:
    b shim_glDeleteShader
.size glDeleteShader, .-glDeleteShader

.globl glDeleteTextures
.type glDeleteTextures, %function
glDeleteTextures:
    b shim_glDeleteTextures
.size glDeleteTextures, .-glDeleteTextures

.globl glDepthFunc
.type glDepthFunc, %function
glDepthFunc:
    b shim_glDepthFunc
.size glDepthFunc, .-glDepthFunc

.globl glDepthMask
.type glDepthMask, %function
glDepthMask:
    b shim_glDepthMask
.size glDepthMask, .-glDepthMask

.globl glDepthRangef
.type glDepthRangef, %function
glDepthRangef:
    b shim_glDepthRangef
.size glDepthRangef, .-glDepthRangef

.globl glDetachShader
.type glDetachShader, %function
glDetachShader:
    b shim_glDetachShader
.size glDetachShader, .-glDetachShader

.globl glDisable
.type glDisable, %function
glDisable:
    b shim_glDisable
.size glDisable, .-glDisable

.globl glDisableVertexAttribArray
.type glDisableVertexAttribArray, %function
glDisableVertexAttribArray:
    b shim_glDisableVertexAttribArray
.size glDisableVertexAttribArray, .-glDisableVertexAttribArray

.globl glDiscardFramebufferEXT
.type glDiscardFramebufferEXT, %function
glDiscardFramebufferEXT:
    b shim_glDiscardFramebufferEXT
.size glDiscardFramebufferEXT, .-glDiscardFramebufferEXT

.globl glDrawArrays
.type glDrawArrays, %function
glDrawArrays:
    b shim_glDrawArrays
.size glDrawArrays, .-glDrawArrays

.globl glDrawElements
.type glDrawElements, %function
glDrawElements:
    b shim_glDrawElements
.size glDrawElements, .-glDrawElements

.globl glEGLImageTargetRenderbufferStorageOES
.type glEGLImageTargetRenderbufferStorageOES, %function
glEGLImageTargetRenderbufferStorageOES:
    b shim_glEGLImageTargetRenderbufferStorageOES
.size glEGLImageTargetRenderbufferStorageOES, .-glEGLImageTargetRenderbufferStorageOES

.globl glEGLImageTargetTexture2DOES
.type glEGLImageTargetTexture2DOES, %function
glEGLImageTargetTexture2DOES:
    b shim_glEGLImageTargetTexture2DOES
.size glEGLImageTargetTexture2DOES, .-glEGLImageTargetTexture2DOES

.globl glEnable
.type glEnable, %function
glEnable:
    b shim_glEnable
.size glEnable, .-glEnable

.globl glEnableVertexAttribArray
.type glEnableVertexAttribArray, %function
glEnableVertexAttribArray:
    b shim_glEnableVertexAttribArray
.size glEnableVertexAttribArray, .-glEnableVertexAttribArray

.globl glFinish
.type glFinish, %function
glFinish:
    b shim_glFinish
.size glFinish, .-glFinish

.globl glFlush
.type glFlush, %function
glFlush:
    b shim_glFlush
.size glFlush, .-glFlush

.globl glFramebufferRenderbuffer
.type glFramebufferRenderbuffer, %function
glFramebufferRenderbuffer:
    b shim_glFramebufferRenderbuffer
.size glFramebufferRenderbuffer, .-glFramebufferRenderbuffer

.globl glFramebufferTexture2D
.type glFramebufferTexture2D, %function
glFramebufferTexture2D:
    b shim_glFramebufferTexture2D
.size glFramebufferTexture2D, .-glFramebufferTexture2D

.globl glFramebufferTexture2DMultisampleEXT
.type glFramebufferTexture2DMultisampleEXT, %function
glFramebufferTexture2DMultisampleEXT:
    b shim_glFramebufferTexture2DMultisampleEXT
.size glFramebufferTexture2DMultisampleEXT, .-glFramebufferTexture2DMultisampleEXT

.globl glFrontFace
.type glFrontFace, %function
glFrontFace:
    b shim_glFrontFace
.size glFrontFace, .-glFrontFace

.globl glGenBuffers
.type glGenBuffers, %function
glGenBuffers:
    b shim_glGenBuffers
.size glGenBuffers, .-glGenBuffers

.globl glGenFramebuffers
.type glGenFramebuffers, %function
glGenFramebuffers:
    b shim_glGenFramebuffers
.size glGenFramebuffers, .-glGenFramebuffers

.globl glGenRenderbuffers
.type glGenRenderbuffers, %function
glGenRenderbuffers:
    b shim_glGenRenderbuffers
.size glGenRenderbuffers, .-glGenRenderbuffers

.globl glGenTextures
.type glGenTextures, %function
glGenTextures:
    b shim_glGenTextures
.size glGenTextures, .-glGenTextures

.globl glGenerateMipmap
.type glGenerateMipmap, %function
glGenerateMipmap:
    b shim_glGenerateMipmap
.size glGenerateMipmap, .-glGenerateMipmap

.globl glGetActiveAttrib
.type glGetActiveAttrib, %function
glGetActiveAttrib:
    b shim_glGetActiveAttrib
.size glGetActiveAttrib, .-glGetActiveAttrib

.globl glGetActiveUniform
.type glGetActiveUniform, %function
glGetActiveUniform:
    b shim_glGetActiveUniform
.size glGetActiveUniform, .-glGetActiveUniform

.globl glGetAttachedShaders
.type glGetAttachedShaders, %function
glGetAttachedShaders:
    b shim_glGetAttachedShaders
.size glGetAttachedShaders, .-glGetAttachedShaders

.globl glGetAttribLocation
.type glGetAttribLocation, %function
glGetAttribLocation:
    b shim_glGetAttribLocation
.size glGetAttribLocation, .-glGetAttribLocation

.globl glGetBooleanv
.type glGetBooleanv, %function
glGetBooleanv:
    b shim_glGetBooleanv
.size glGetBooleanv, .-glGetBooleanv

.globl glGetBufferParameteriv
.type glGetBufferParameteriv, %function
glGetBufferParameteriv:
    b shim_glGetBufferParameteriv
.size glGetBufferParameteriv, .-glGetBufferParameteriv

.globl glGetError
.type glGetError, %function
glGetError:
    b shim_glGetError
.size glGetError, .-glGetError

.globl glGetFloatv
.type glGetFloatv, %function
glGetFloatv:
    b shim_glGetFloatv
.size glGetFloatv, .-glGetFloatv

.globl glGetFramebufferAttachmentParameteriv
.type glGetFramebufferAttachmentParameteriv, %function
glGetFramebufferAttachmentParameteriv:
    b shim_glGetFramebufferAttachmentParameteriv
.size glGetFramebufferAttachmentParameteriv, .-glGetFramebufferAttachmentParameteriv

.globl glGetGraphicsResetStatusEXT
.type glGetGraphicsResetStatusEXT, %function
glGetGraphicsResetStatusEXT:
    b shim_glGetGraphicsResetStatusEXT
.size glGetGraphicsResetStatusEXT, .-glGetGraphicsResetStatusEXT

.globl glGetIntegerv
.type glGetIntegerv, %function
glGetIntegerv:
    b shim_glGetIntegerv
.size glGetIntegerv, .-glGetIntegerv

.globl glGetProgramBinaryOES
.type glGetProgramBinaryOES, %function
glGetProgramBinaryOES:
    b shim_glGetProgramBinaryOES
.size glGetProgramBinaryOES, .-glGetProgramBinaryOES

.globl glGetProgramInfoLog
.type glGetProgramInfoLog, %function
glGetProgramInfoLog:
    b shim_glGetProgramInfoLog
.size glGetProgramInfoLog, .-glGetProgramInfoLog

.globl glGetProgramiv
.type glGetProgramiv, %function
glGetProgramiv:
    b shim_glGetProgramiv
.size glGetProgramiv, .-glGetProgramiv

.globl glGetRenderbufferParameteriv
.type glGetRenderbufferParameteriv, %function
glGetRenderbufferParameteriv:
    b shim_glGetRenderbufferParameteriv
.size glGetRenderbufferParameteriv, .-glGetRenderbufferParameteriv

.globl glGetShaderInfoLog
.type glGetShaderInfoLog, %function
glGetShaderInfoLog:
    b shim_glGetShaderInfoLog
.size glGetShaderInfoLog, .-glGetShaderInfoLog

.globl glGetShaderSource
.type glGetShaderSource, %function
glGetShaderSource:
    b shim_glGetShaderSource
.size glGetShaderSource, .-glGetShaderSource

.globl glGetShaderiv
.type glGetShaderiv, %function
glGetShaderiv:
    b shim_glGetShaderiv
.size glGetShaderiv, .-glGetShaderiv

.globl glGetString
.type glGetString, %function
glGetString:
    b shim_glGetString
.size glGetString, .-glGetString

.globl glGetTexParameterfv
.type glGetTexParameterfv, %function
glGetTexParameterfv:
    b shim_glGetTexParameterfv
.size glGetTexParameterfv, .-glGetTexParameterfv

.globl glGetTexParameteriv
.type glGetTexParameteriv, %function
glGetTexParameteriv:
    b shim_glGetTexParameteriv
.size glGetTexParameteriv, .-glGetTexParameteriv

.globl glGetUniformLocation
.type glGetUniformLocation, %function
glGetUniformLocation:
    b shim_glGetUniformLocation
.size glGetUniformLocation, .-glGetUniformLocation

.globl glGetUniformfv
.type glGetUniformfv, %function
glGetUniformfv:
    b shim_glGetUniformfv
.size glGetUniformfv, .-glGetUniformfv

.globl glGetUniformiv
.type glGetUniformiv, %function
glGetUniformiv:
    b shim_glGetUniformiv
.size glGetUniformiv, .-glGetUniformiv

.globl glGetVertexAttribPointerv
.type glGetVertexAttribPointerv, %function
glGetVertexAttribPointerv:
    b shim_glGetVertexAttribPointerv
.size glGetVertexAttribPointerv, .-glGetVertexAttribPointerv

.globl glGetVertexAttribfv
.type glGetVertexAttribfv, %function
glGetVertexAttribfv:
    b shim_glGetVertexAttribfv
.size glGetVertexAttribfv, .-glGetVertexAttribfv

.globl glGetVertexAttribiv
.type glGetVertexAttribiv, %function
glGetVertexAttribiv:
    b shim_glGetVertexAttribiv
.size glGetVertexAttribiv, .-glGetVertexAttribiv

.globl glGetnUniformfvEXT
.type glGetnUniformfvEXT, %function
glGetnUniformfvEXT:
    b shim_glGetnUniformfvEXT
.size glGetnUniformfvEXT, .-glGetnUniformfvEXT

.globl glGetnUniformivEXT
.type glGetnUniformivEXT, %function
glGetnUniformivEXT:
    b shim_glGetnUniformivEXT
.size glGetnUniformivEXT, .-glGetnUniformivEXT

.globl glHint
.type glHint, %function
glHint:
    b shim_glHint
.size glHint, .-glHint

.globl glIsBuffer
.type glIsBuffer, %function
glIsBuffer:
    b shim_glIsBuffer
.size glIsBuffer, .-glIsBuffer

.globl glIsEnabled
.type glIsEnabled, %function
glIsEnabled:
    b shim_glIsEnabled
.size glIsEnabled, .-glIsEnabled

.globl glIsFramebuffer
.type glIsFramebuffer, %function
glIsFramebuffer:
    b shim_glIsFramebuffer
.size glIsFramebuffer, .-glIsFramebuffer

.globl glIsProgram
.type glIsProgram, %function
glIsProgram:
    b shim_glIsProgram
.size glIsProgram, .-glIsProgram

.globl glIsRenderbuffer
.type glIsRenderbuffer, %function
glIsRenderbuffer:
    b shim_glIsRenderbuffer
.size glIsRenderbuffer, .-glIsRenderbuffer

.globl glIsShader
.type glIsShader, %function
glIsShader:
    b shim_glIsShader
.size glIsShader, .-glIsShader

.globl glIsTexture
.type glIsTexture, %function
glIsTexture:
    b shim_glIsTexture
.size glIsTexture, .-glIsTexture

.globl glLineWidth
.type glLineWidth, %function
glLineWidth:
    b shim_glLineWidth
.size glLineWidth, .-glLineWidth

.globl glLinkProgram
.type glLinkProgram, %function
glLinkProgram:
    b shim_glLinkProgram
.size glLinkProgram, .-glLinkProgram

.globl glPixelStorei
.type glPixelStorei, %function
glPixelStorei:
    b shim_glPixelStorei
.size glPixelStorei, .-glPixelStorei

.globl glPolygonOffset
.type glPolygonOffset, %function
glPolygonOffset:
    b shim_glPolygonOffset
.size glPolygonOffset, .-glPolygonOffset

.globl glProgramBinaryOES
.type glProgramBinaryOES, %function
glProgramBinaryOES:
    b shim_glProgramBinaryOES
.size glProgramBinaryOES, .-glProgramBinaryOES

.globl glReadPixels
.type glReadPixels, %function
glReadPixels:
    b shim_glReadPixels
.size glReadPixels, .-glReadPixels

.globl glReadnPixelsEXT
.type glReadnPixelsEXT, %function
glReadnPixelsEXT:
    b shim_glReadnPixelsEXT
.size glReadnPixelsEXT, .-glReadnPixelsEXT

.globl glReleaseShaderCompiler
.type glReleaseShaderCompiler, %function
glReleaseShaderCompiler:
    b shim_glReleaseShaderCompiler
.size glReleaseShaderCompiler, .-glReleaseShaderCompiler

.globl glRenderbufferStorage
.type glRenderbufferStorage, %function
glRenderbufferStorage:
    b shim_glRenderbufferStorage
.size glRenderbufferStorage, .-glRenderbufferStorage

.globl glRenderbufferStorageMultisampleEXT
.type glRenderbufferStorageMultisampleEXT, %function
glRenderbufferStorageMultisampleEXT:
    b shim_glRenderbufferStorageMultisampleEXT
.size glRenderbufferStorageMultisampleEXT, .-glRenderbufferStorageMultisampleEXT

.globl glSampleCoverage
.type glSampleCoverage, %function
glSampleCoverage:
    b shim_glSampleCoverage
.size glSampleCoverage, .-glSampleCoverage

.globl glScissor
.type glScissor, %function
glScissor:
    b shim_glScissor
.size glScissor, .-glScissor

.globl glShaderBinary
.type glShaderBinary, %function
glShaderBinary:
    b shim_glShaderBinary
.size glShaderBinary, .-glShaderBinary

.globl glShaderSource
.type glShaderSource, %function
glShaderSource:
    b shim_glShaderSource
.size glShaderSource, .-glShaderSource

.globl glStencilFunc
.type glStencilFunc, %function
glStencilFunc:
    b shim_glStencilFunc
.size glStencilFunc, .-glStencilFunc

.globl glStencilFuncSeparate
.type glStencilFuncSeparate, %function
glStencilFuncSeparate:
    b shim_glStencilFuncSeparate
.size glStencilFuncSeparate, .-glStencilFuncSeparate

.globl glStencilMask
.type glStencilMask, %function
glStencilMask:
    b shim_glStencilMask
.size glStencilMask, .-glStencilMask

.globl glStencilMaskSeparate
.type glStencilMaskSeparate, %function
glStencilMaskSeparate:
    b shim_glStencilMaskSeparate
.size glStencilMaskSeparate, .-glStencilMaskSeparate

.globl glStencilOp
.type glStencilOp, %function
glStencilOp:
    b shim_glStencilOp
.size glStencilOp, .-glStencilOp

.globl glStencilOpSeparate
.type glStencilOpSeparate, %function
glStencilOpSeparate:
    b shim_glStencilOpSeparate
.size glStencilOpSeparate, .-glStencilOpSeparate

.globl glTexImage2D
.type glTexImage2D, %function
glTexImage2D:
    b shim_glTexImage2D
.size glTexImage2D, .-glTexImage2D

.globl glTexParameterf
.type glTexParameterf, %function
glTexParameterf:
    b shim_glTexParameterf
.size glTexParameterf, .-glTexParameterf

.globl glTexParameterfv
.type glTexParameterfv, %function
glTexParameterfv:
    b shim_glTexParameterfv
.size glTexParameterfv, .-glTexParameterfv

.globl glTexParameteri
.type glTexParameteri, %function
glTexParameteri:
    b shim_glTexParameteri
.size glTexParameteri, .-glTexParameteri

.globl glTexParameteriv
.type glTexParameteriv, %function
glTexParameteriv:
    b shim_glTexParameteriv
.size glTexParameteriv, .-glTexParameteriv

.globl glTexSubImage2D
.type glTexSubImage2D, %function
glTexSubImage2D:
    b shim_glTexSubImage2D
.size glTexSubImage2D, .-glTexSubImage2D

.globl glUniform1f
.type glUniform1f, %function
glUniform1f:
    b shim_glUniform1f
.size glUniform1f, .-glUniform1f

.globl glUniform1fv
.type glUniform1fv, %function
glUniform1fv:
    b shim_glUniform1fv
.size glUniform1fv, .-glUniform1fv

.globl glUniform1i
.type glUniform1i, %function
glUniform1i:
    b shim_glUniform1i
.size glUniform1i, .-glUniform1i

.globl glUniform1iv
.type glUniform1iv, %function
glUniform1iv:
    b shim_glUniform1iv
.size glUniform1iv, .-glUniform1iv

.globl glUniform2f
.type glUniform2f, %function
glUniform2f:
    b shim_glUniform2f
.size glUniform2f, .-glUniform2f

.globl glUniform2fv
.type glUniform2fv, %function
glUniform2fv:
    b shim_glUniform2fv
.size glUniform2fv, .-glUniform2fv

.globl glUniform2i
.type glUniform2i, %function
glUniform2i:
    b shim_glUniform2i
.size glUniform2i, .-glUniform2i

.globl glUniform2iv
.type glUniform2iv, %function
glUniform2iv:
    b shim_glUniform2iv
.size glUniform2iv, .-glUniform2iv

.globl glUniform3f
.type glUniform3f, %function
glUniform3f:
    b shim_glUniform3f
.size glUniform3f, .-glUniform3f

.globl glUniform3fv
.type glUniform3fv, %function
glUniform3fv:
    b shim_glUniform3fv
.size glUniform3fv, .-glUniform3fv

.globl glUniform3i
.type glUniform3i, %function
glUniform3i:
    b shim_glUniform3i
.size glUniform3i, .-glUniform3i

.globl glUniform3iv
.type glUniform3iv, %function
glUniform3iv:
    b shim_glUniform3iv
.size glUniform3iv, .-glUniform3iv

.globl glUniform4f
.type glUniform4f, %function
glUniform4f:
    b shim_glUniform4f
.size glUniform4f, .-glUniform4f

.globl glUniform4fv
.type glUniform4fv, %function
glUniform4fv:
    b shim_glUniform4fv
.size glUniform4fv, .-glUniform4fv

.globl glUniform4i
.type glUniform4i, %function
glUniform4i:
    b shim_glUniform4i
.size glUniform4i, .-glUniform4i

.globl glUniform4iv
.type glUniform4iv, %function
glUniform4iv:
    b shim_glUniform4iv
.size glUniform4iv, .-glUniform4iv

.globl glUniformMatrix2fv
.type glUniformMatrix2fv, %function
glUniformMatrix2fv:
    b shim_glUniformMatrix2fv
.size glUniformMatrix2fv, .-glUniformMatrix2fv

.globl glUniformMatrix3fv
.type glUniformMatrix3fv, %function
glUniformMatrix3fv:
    b shim_glUniformMatrix3fv
.size glUniformMatrix3fv, .-glUniformMatrix3fv

.globl glUniformMatrix4fv
.type glUniformMatrix4fv, %function
glUniformMatrix4fv:
    b shim_glUniformMatrix4fv
.size glUniformMatrix4fv, .-glUniformMatrix4fv

.globl glUseProgram
.type glUseProgram, %function
glUseProgram:
    b shim_glUseProgram
.size glUseProgram, .-glUseProgram

.globl glValidateProgram
.type glValidateProgram, %function
glValidateProgram:
    b shim_glValidateProgram
.size glValidateProgram, .-glValidateProgram

.globl glVertexAttrib1f
.type glVertexAttrib1f, %function
glVertexAttrib1f:
    b shim_glVertexAttrib1f
.size glVertexAttrib1f, .-glVertexAttrib1f

.globl glVertexAttrib1fv
.type glVertexAttrib1fv, %function
glVertexAttrib1fv:
    b shim_glVertexAttrib1fv
.size glVertexAttrib1fv, .-glVertexAttrib1fv

.globl glVertexAttrib2f
.type glVertexAttrib2f, %function
glVertexAttrib2f:
    b shim_glVertexAttrib2f
.size glVertexAttrib2f, .-glVertexAttrib2f

.globl glVertexAttrib2fv
.type glVertexAttrib2fv, %function
glVertexAttrib2fv:
    b shim_glVertexAttrib2fv
.size glVertexAttrib2fv, .-glVertexAttrib2fv

.globl glVertexAttrib3f
.type glVertexAttrib3f, %function
glVertexAttrib3f:
    b shim_glVertexAttrib3f
.size glVertexAttrib3f, .-glVertexAttrib3f

.globl glVertexAttrib3fv
.type glVertexAttrib3fv, %function
glVertexAttrib3fv:
    b shim_glVertexAttrib3fv
.size glVertexAttrib3fv, .-glVertexAttrib3fv

.globl glVertexAttrib4f
.type glVertexAttrib4f, %function
glVertexAttrib4f:
    b shim_glVertexAttrib4f
.size glVertexAttrib4f, .-glVertexAttrib4f

.globl glVertexAttrib4fv
.type glVertexAttrib4fv, %function
glVertexAttrib4fv:
    b shim_glVertexAttrib4fv
.size glVertexAttrib4fv, .-glVertexAttrib4fv

.globl glVertexAttribPointer
.type glVertexAttribPointer, %function
glVertexAttribPointer:
    b shim_glVertexAttribPointer
.size glVertexAttribPointer, .-glVertexAttribPointer

.globl glViewport
.type glViewport, %function
glViewport:
    b shim_glViewport
.size glViewport, .-glViewport
