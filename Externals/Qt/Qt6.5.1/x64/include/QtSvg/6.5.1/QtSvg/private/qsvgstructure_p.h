// Copyright (C) 2016 The Qt Company Ltd.
// SPDX-License-Identifier: LicenseRef-Qt-Commercial OR LGPL-3.0-only OR GPL-2.0-only OR GPL-3.0-only

#ifndef QSVGSTRUCTURE_P_H
#define QSVGSTRUCTURE_P_H

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

#include "qsvgnode_p.h"
#include "qtsvgglobal_p.h"

#include "QtCore/qlist.h"
#include "QtCore/qhash.h"

QT_BEGIN_NAMESPACE

class QSvgTinyDocument;
class QSvgNode;
class QPainter;
class QSvgDefs;

class Q_SVG_PRIVATE_EXPORT QSvgStructureNode : public QSvgNode
{
public:
    QSvgStructureNode(QSvgNode *parent);
    ~QSvgStructureNode();
    QSvgNode *scopeNode(const QString &id) const;
    void addChild(QSvgNode *child, const QString &id);
    QRectF bounds(QPainter *p, QSvgExtraStates &states) const override;
    QSvgNode *previousSiblingNode(QSvgNode *n) const;
    QList<QSvgNode*> renderers() const { return m_renderers; }
protected:
    QList<QSvgNode*>          m_renderers;
    QHash<QString, QSvgNode*> m_scope;
    QList<QSvgStructureNode*> m_linkedScopes;
    mutable bool              m_recursing = false;
};

class Q_SVG_PRIVATE_EXPORT QSvgG : public QSvgStructureNode
{
public:
    QSvgG(QSvgNode *parent);
    void draw(QPainter *p, QSvgExtraStates &states) override;
    Type type() const override;
};

class Q_SVG_PRIVATE_EXPORT QSvgDefs : public QSvgStructureNode
{
public:
    QSvgDefs(QSvgNode *parent);
    void draw(QPainter *p, QSvgExtraStates &states) override;
    Type type() const override;
};

class Q_SVG_PRIVATE_EXPORT QSvgSwitch : public QSvgStructureNode
{
public:
    QSvgSwitch(QSvgNode *parent);
    void draw(QPainter *p, QSvgExtraStates &states) override;
    Type type() const override;
private:
    void init();
private:
    QString m_systemLanguage;
    QString m_systemLanguagePrefix;
};

QT_END_NAMESPACE

#endif // QSVGSTRUCTURE_P_H
