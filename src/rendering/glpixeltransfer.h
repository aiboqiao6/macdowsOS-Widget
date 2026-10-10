#pragma once
#include "rendering/renderdiagnostics.h"
#include <QOpenGLContext>
#include <QOpenGLFunctions>

// 保护像素传输状态，防止 CPU 指针被当作 PBO 偏移，或把图像行填充误读为像素。
class PixelTransferScope final {
public:
    PixelTransferScope(QOpenGLFunctions* gl, bool upload, int rowLength = 0)
        : m_gl(gl), m_bufferTarget(upload ? GL_PIXEL_UNPACK_BUFFER : GL_PIXEL_PACK_BUFFER) {
        const GLenum states[] = {
            GLenum(upload ? GL_UNPACK_ALIGNMENT : GL_PACK_ALIGNMENT),
            GLenum(upload ? GL_UNPACK_ROW_LENGTH : GL_PACK_ROW_LENGTH),
            GLenum(upload ? GL_UNPACK_SKIP_PIXELS : GL_PACK_SKIP_PIXELS),
            GLenum(upload ? GL_UNPACK_SKIP_ROWS : GL_PACK_SKIP_ROWS)
        };
        for (int i = 0; i < 4; ++i) {
            m_states[i] = states[i];
            m_gl->glGetIntegerv(m_states[i], &m_values[i]);
            m_gl->glPixelStorei(m_states[i], i == 0 ? 4 : (i == 1 ? rowLength : 0));
        }
        m_gl->glGetIntegerv(upload ? GL_PIXEL_UNPACK_BUFFER_BINDING : GL_PIXEL_PACK_BUFFER_BINDING,
                           &m_buffer);
        m_gl->glBindBuffer(m_bufferTarget, 0);
        if (RenderDiagnostics::enabled()
            && (m_buffer || m_values[0] != 4 || m_values[1] || m_values[2] || m_values[3])) {
            RenderDiagnostics::write(QString("gl.transfer-state/0x%1/%2")
                .arg(quintptr(QOpenGLContext::currentContext()), 0, 16).arg(upload ? "upload" : "read"),
                QString("inheritedPbo=%1 alignment=%2 rowLength=%3 skipPixels=%4 skipRows=%5 usedRowLength=%6")
                    .arg(m_buffer).arg(m_values[0]).arg(m_values[1]).arg(m_values[2]).arg(m_values[3]).arg(rowLength), 1000);
        }
    }
    ~PixelTransferScope() {
        m_gl->glBindBuffer(m_bufferTarget, GLuint(m_buffer));
        for (int i = 0; i < 4; ++i)
            m_gl->glPixelStorei(m_states[i], m_values[i]);
    }
private:
    QOpenGLFunctions* m_gl;
    GLenum m_bufferTarget;
    GLint m_buffer = 0;
    GLenum m_states[4];
    GLint m_values[4];
};

