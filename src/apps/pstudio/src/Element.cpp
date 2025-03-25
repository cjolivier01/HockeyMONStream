#include "Element.h"

#include <QStringList>
#include <QRegularExpression>

Element::Element(const QString &factoryName, const QString &name)
    : m_name(name)
    , m_factoryName(factoryName)
    , m_position(0, 0)
{
}

Element::~Element()
{
}

QString Element::name() const
{
    return m_name;
}

void Element::setName(const QString &name)
{
    m_name = name;
}

QString Element::factoryName() const
{
    return m_factoryName;
}

QPointF Element::position() const
{
    return m_position;
}

void Element::setPosition(const QPointF &pos)
{
    m_position = pos;
}

QMap<QString, QVariant> Element::properties() const
{
    return m_properties;
}

QVariant Element::property(const QString &name) const
{
    return m_properties.value(name);
}

void Element::setProperty(const QString &name, const QVariant &value)
{
    m_properties[name] = value;
}

QString Element::toString() const
{
    QString result = m_name + "=" + m_factoryName;
    
    // Add position
    result += QString(" @pos(%1,%2)").arg(m_position.x()).arg(m_position.y());
    
    // Add properties
    for (auto it = m_properties.constBegin(); it != m_properties.constEnd(); ++it) {
        // Skip empty properties
        if (it.value().toString().isEmpty())
            continue;
            
        result += " " + it.key() + "=" + it.value().toString();
    }
    
    return result;
}

Element* Element::fromString(const QString &str)
{
    // Parse element string
    QRegularExpression re("^([^=]+)=([^ ]+)(.*)$");
    QRegularExpressionMatch match = re.match(str);
    
    if (!match.hasMatch())
        return nullptr;
        
    QString name = match.captured(1);
    QString factoryName = match.captured(2);
    QString rest = match.captured(3).trimmed();
    
    Element *element = new Element(factoryName, name);
    
    // Parse position if available
    QRegularExpression posRe("@pos\\(([^,]+),([^)]+)\\)");
    QRegularExpressionMatch posMatch = posRe.match(rest);
    
    if (posMatch.hasMatch()) {
        double x = posMatch.captured(1).toDouble();
        double y = posMatch.captured(2).toDouble();
        element->setPosition(QPointF(x, y));
        
        // Remove position part from rest
        rest.replace(posMatch.captured(0), "");
        rest = rest.trimmed();
    }
    
    // Parse properties
    QStringList props = rest.split(" ", Qt::SkipEmptyParts);
    
    for (const QString &prop : props) {
        QStringList keyValue = prop.split("=");
        
        if (keyValue.size() == 2) {
            QString key = keyValue[0];
            QString value = keyValue[1];
            
            element->setProperty(key, value);
        }
    }
    
    return element;
}
