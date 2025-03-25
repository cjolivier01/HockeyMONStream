#include "Pipeline.h"
#include "Connection.h"
#include "Element.h"

#include <QRegularExpression>
#include <QStringList>

Pipeline::Pipeline() {}

Pipeline::~Pipeline() {
  // Delete all elements
  for (auto it = m_elements.begin(); it != m_elements.end(); ++it) {
    delete it.value();
  }

  // Delete all connections
  for (auto connection : m_connections) {
    delete connection;
  }
}

bool Pipeline::addElement(Element* element) {
  if (!element)
    return false;

  QString name = element->name();

  if (m_elements.contains(name))
    return false;

  m_elements[name] = element;
  return true;
}

bool Pipeline::removeElement(const QString& name) {
  if (!m_elements.contains(name))
    return false;

  // Remove connections involving this element
  QList<Connection*> connectionsToRemove;

  for (Connection* connection : m_connections) {
    if (connection->sourceElement() == name || connection->destinationElement() == name) {
      connectionsToRemove.append(connection);
    }
  }

  for (Connection* connection : connectionsToRemove) {
    removeConnection(connection);
  }

  // Remove the element
  Element* element = m_elements.take(name);
  delete element;

  return true;
}

Element* Pipeline::element(const QString& name) const {
  return m_elements.value(name);
}

QList<Element*> Pipeline::elements() const {
  return m_elements.values();
}

bool Pipeline::addConnection(Connection* connection) {
  if (!connection)
    return false;

  // Check if elements exist
  if (!m_elements.contains(connection->sourceElement()) || !m_elements.contains(connection->destinationElement()))
    return false;

  // Check if connection already exists
  for (Connection* existingConn : m_connections) {
    if (*existingConn == *connection)
      return false;
  }

  m_connections.append(connection);
  return true;
}

bool Pipeline::removeConnection(Connection* connection) {
  if (!connection)
    return false;

  int index = m_connections.indexOf(connection);

  if (index == -1)
    return false;

  m_connections.removeAt(index);
  delete connection;

  return true;
}

QList<Connection*> Pipeline::connections() const {
  return m_connections;
}

QString Pipeline::toString() const {
  QString result;

  // Add elements
  for (const Element* element : m_elements.values()) {
    result += element->toString() + "\n";
  }

  result += "\n";

  // Add connections
  for (const Connection* connection : m_connections) {
    result += connection->toString() + "\n";
  }

  return result;
}

Pipeline* Pipeline::fromString(const QString& str) {
  Pipeline* pipeline = new Pipeline();

  QStringList lines = str.split("\n", Qt::SkipEmptyParts);

  // Process each line
  for (const QString& line : lines) {
    // Check if it's an element definition
    if (line.contains("=") && !line.contains("->")) {
      Element* element = Element::fromString(line);

      if (element) {
        pipeline->addElement(element);
      }
    }
    // Check if it's a connection definition
    else if (line.contains("->")) {
      Connection* connection = Connection::fromString(line);

      if (connection) {
        pipeline->addConnection(connection);
      }
    }
  }

  return pipeline;
}
