#ifndef ELEMENT_H
#define ELEMENT_H

#include <QtCore/QString>
#include <QtCore/QPointF>
#include <QtCore/QMap>
#include <QtCore/QVariant>

class Element
{
public:
    Element(const QString &factoryName, const QString &name);
    ~Element();

    // Basic properties
    QString name() const;
    void setName(const QString &name);
    
    QString factoryName() const;
    
    // Visual properties for the editor
    QPointF position() const;
    void setPosition(const QPointF &pos);
    
    // Element properties
    QMap<QString, QVariant> properties() const;
    QVariant property(const QString &name) const;
    void setProperty(const QString &name, const QVariant &value);
    
    // Serialization
    QString toString() const;
    static Element* fromString(const QString &str);

private:
    QString m_name;
    QString m_factoryName;
    QPointF m_position;
    QMap<QString, QVariant> m_properties;
};

#endif // ELEMENT_H
