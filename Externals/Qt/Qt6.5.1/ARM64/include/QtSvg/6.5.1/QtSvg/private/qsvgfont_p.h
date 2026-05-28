// Copyright (C) 2016 The Qt Company Ltd.
// SPDX-License-Identifier: LicenseRef-Qt-Commercial OR LGPL-3.0-only OR GPL-2.0-only OR GPL-3.0-only

#ifndef QSVGFONT_P_H
#define QSVGFONT_P_H

//
//  W A R N I N G
//  -------------
//
// This file is not part of the Qt API.  It exists purely as an
// implementation detail.  This header file may change from version to
// version without notice, or even be removed.
//
// We mean it.
//

#include "qpainterpath.h"
#include "qhash.h"
#include "qstring.h"
#include "qsvgstyle_p.h"
#include "qtsvgglobal_p.h"

QT_BEGIN_NAMESPACE

class Q_SVG_PRIVATE_EXPORT QSvgGlyph
{
public:
    QSvgGlyph(QChar unicode, const QPainterPath &path, qreal horizAdvX);
    QSvgGlyph() : m_unicode(0), m_horizAdvX(0) {}

    QChar m_unicode;
    QPainterPath m_path;
    qreal m_horizAdvX;
};


class Q_SVG_PRIVATE_EXPORT QSvgFont : public QSvgRefCounted
{
public:
    static constexpr qreal DEFAULT_UNITS_PER_EM = 1000;
    QSvgFont(qreal horizAdvX);

    void setFamilyName(const QString &name);
    QString familyName() const;

    void setUnitsPerEm(qreal upem);

    void addGlyph(QChar unicode, const QPainterPath &path, qreal horizAdvX = -1);

    void draw(QPainter *p, const QPointF &point, const QString &str, qreal pixelSize, Qt::Alignment alignment) const;
public:
    QString m_familyName;
    qreal m_unitsPerEm = DEFAULT_UNITS_PER_EM;
    qreal m_horizAdvX;
    QHash<QChar, QSvgGlyph> m_glyphs;
};

QT_END_NAMESPACE

#endif // QSVGFONT_P_H
