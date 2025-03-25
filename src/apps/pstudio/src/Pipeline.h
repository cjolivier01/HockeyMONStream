#ifndef PIPELINE_H
#define PIPELINE_H

#include <QList>
#include <QMap>
#include <QString>

// Forward declarations
class Element;
class Connection;

class Pipeline {
 public:
  Pipeline();
  ~Pipeline();

  // Element management
  bool addElement(Element* element);
  bool removeElement(const QString& name);
  Element* element(const QString& name) const;
  QList<Element*> elements() const;

  // Connection management
  bool addConnection(Connection* connection);
  bool removeConnection(Connection* connection);
  QList<Connection*> connections() const;

  // Serialization
  QString toString() const;
  static Pipeline* fromString(const QString& str);

 private:
  QMap<QString, Element*> m_elements;
  QList<Connection*> m_connections;
};

#endif // PIPELINE_H
