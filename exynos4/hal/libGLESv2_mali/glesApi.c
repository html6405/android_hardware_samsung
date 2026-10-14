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

#include <string.h>

#include <GLES2/gl2.h>
#include <log/log.h>

/* Forward the stock GLES2 ABI to libMali.so; wrap only the precision query. */
extern void shim_glGetShaderPrecisionFormat(GLenum shadertype, GLenum precisiontype,
                                            GLint *range, GLint *precision);
extern const GLubyte *shim_glGetString(GLenum name);

static int is_mali400_renderer(void)
{
    const GLubyte *renderer = shim_glGetString(GL_RENDERER);

    return renderer != NULL && strstr((const char *)renderer, "Mali-400") != NULL;
}

static int meets_es2_highp_float_minimum(const GLint *range, GLint precision)
{
    return range != NULL && range[0] >= 62 && range[1] >= 62 && precision >= 16;
}

/*
 * Mali-400 reports no fragment highp, though r3p2 compiles it as mediump.
 * ANGLE rejects that report, so normalize only fragment HIGH_FLOAT on Mali-400.
 * Leave all other driver capabilities unchanged.
 */
void glGetShaderPrecisionFormat(GLenum shadertype, GLenum precisiontype,
                                GLint *range, GLint *precision)
{
    static int logged;
    GLint originalRange[2] = {0, 0};
    GLint originalPrecision = 0;

    shim_glGetShaderPrecisionFormat(shadertype, precisiontype, range, precision);

    if (range == NULL || precision == NULL)
        return;

    if (shadertype != GL_FRAGMENT_SHADER || precisiontype != GL_HIGH_FLOAT)
        return;

    if (!is_mali400_renderer())
        return;

    if (meets_es2_highp_float_minimum(range, *precision))
        return;

    originalRange[0] = range[0];
    originalRange[1] = range[1];
    originalPrecision = *precision;

    range[0] = 62;
    range[1] = 62;
    *precision = 16;

    if (!logged) {
        ALOGW("Mali-400 legacy GLES2 compatibility: fragment highp %d/%d/%d -> 62/62/16; "
              "r3p2 compiler executes fragment highp at mediump precision",
              originalRange[0], originalRange[1], originalPrecision);
        logged = 1;
    }
}
