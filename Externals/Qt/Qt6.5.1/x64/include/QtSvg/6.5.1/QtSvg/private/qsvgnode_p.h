// Copyright (C) 2016 The Qt Company Ltd.
// SPDX-License-Identifier: LicenseRef-Qt-Commercial OR LGPL-3.0-only OR GPL-2.0-only OR GPL-3.0-only

#ifndef QSVGNODE_P_H
#define QSVGNODE_P_H

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

#include "qsvgstyle_p.h"
#include "qtsvgglobal_p.h"

#include "QtCore/qstring.h"
#include "QtCore/qhash.h"

QT_BEGIN_NAMESPACE

class QPainter;
class QSvgTinyDocument;

class Q_SVG_PRIVATE_EXPORT QSvgNode
{
public:
    enum Type
    {
        DOC,
        G,
        DEFS,
        SWITCH,
        ANIMATION,
        ARC,
        CIRCLE,
        ELLIPSE,
        IMAGE,
        LINE,
        PATH,
        POLYGON,
        POLYLINE,
        RECT,
        TEXT,
        TEXTAREA,
        TSPAN,
        USE,
        VIDEO
    };
    enum DisplayMode {
        InlineMode,
        BlockMode,
        ListItemMode,
        RunInMode,
        CompactMode,
        MarkerMode,
        TableMode,
        InlineTableMode,
        TableRowGroupMode,
        TableHeaderGroupMode,
        TableFooterGroupMode,
        TableRowMode,
        TableColumnGroupMode,
        TableColumnMode,
        TableCellMode,
        TableCaptionMode,
        NoneMode,
        InheritMode
    };
public:
    QSvgNode(QSvgNode *parent=0);
    virtual ~QSvgNode();
    virtual void draw(QPainter *p, QSvgExtraStates &states) =0;

    QSvgNode *parent() const;
    bool isDescendantOf(const QSvgNode *parent) const;

    void appendStyleProperty(QSvgStyleProperty *prop, const QString &id);
    void applyStyle(QPainter *p, QSvgExtraStates &states) const;
    void revertStyle(QPainter *p, QSvgExtraStates &states) const;
    QSvgStyleProperty *styleProperty(QSvgStyleProperty::Type type) const;
    QSvgFillStyleProperty *styleProperty(const QString &id) const;

    QSvgTinyDocument *document() const;

    virtual Type type() const =0;
    virtual QRectF fastBounds(QPainter *p, QSvgExtraStates &states) const;
    virtual QRectF bounds(QPainter *p, QSvgExtraStates &states) const;
    virtual QRectF transformedBounds(QPainter *p, QSvgExtraStates &states) const;
    QRectF transformedBounds() const;

    void setRequiredFeatures(const QStringList &lst);
    const QStringList & requiredFeatures() const;

    void setRequiredExtensions(const QStringList &lst);
    const QStringList & requiredExtensions() const;

    void setRequiredLanguages(const QStringList &lst);
    const QStringList & requiredLanguages() const;

    void setRequiredFormats(const QStringList &lst);
    const QStringList & requiredFormats() const;

    void setRequiredFonts(const QStringList &lst);
    const QStringList & requiredFonts() const;

    void setVisible(bool visible);
    bool isVisible() const;

    void setDisplayMode(DisplayMode display);
    DisplayMode displayMode() const;

    QString nodeId() const;
    void setNodeId(const QString &i);

    QString xmlClass() const;
    void setXmlClass(const QString &str);

    bool shouldDrawNode(QPainter *p, QSvgExtraStates &states) const;
protected:
    mutable QSvgStyle m_style;

    static qreal strokeWidth(QPainter *p);
private:
    QSvgNode   *m_parent;

    QStringList m_requiredFeatures;
    QStringList m_requiredExtensions;
    QStringList m_requiredLanguages;
    QStringList m_requiredFormats;
    QStringList m_requiredFonts;

    bool        m_visible;

    QString m_id;
    QString m_class;

    DisplayMode m_displayMode;
    mutable QRectF m_cachedBounds;

    friend class QSvgTinyDocument;
};

inline QSvgNode *QSvgNode::parent() const
{
    return m_parent;
}

inline bool QSvgNode::isVisible() const
{
    return m_visible;
}

inline QString QSvgNode::nodeId() const
{
    return m_id;
}

inline QString QSvgNode::xmlClass() const
{
    return m_class;
}

QT_END_NAMESPACE

#endif // QSVGNODE_P_H
