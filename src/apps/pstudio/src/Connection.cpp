#include "Connection.h"

#include <QRegularExpression>

Connection::Connection(
    const QString& srcElement,
    const QString& srcPad,
    const QString& dstElement,
    const QString& dstPad)
    : m_srcElement(srcElement), m_srcPad(srcPad), m_dstElement(dstElement), m_dstPad(dstPad) {}

Connection::~Connection() {}

QString Connection::sourceElement() const {
  return m_srcElement;
}

QString Connection::sourcePad() const {
  return m_srcPad;
}

QString Connection::destinationElement() const {
  return m_dstElement;
}

QString Connection::destinationPad() const {
  return m_dstPad;
}

bool Connection::operator==(const Connection& other) const {
  return m_srcElement == other.m_srcElement && m_srcPad == other.m_srcPad && m_dstElement == other.m_dstElement &&
      m_dstPad == other.m_dstPad;
}

QString Connection::toString() const {
  return m_srcElement + "." + m_srcPad + " -> " + m_dstElement + "." + m_dstPad;
}

Connection* Connection::fromString(const QString& str) {
  // Parse connection string
  QRegularExpression re("^([^.]+)\\.([^ ]+) -> ([^.]+)\\.([^ ]+)$");
  QRegularExpressionMatch match = re.match(str);

  if (!match.hasMatch())
    return nullptr;

  QString srcElement = match.captured(1);
  QString srcPad = match.captured(2);
  QString dstElement = match.captured(3);
  QString dstPad = match.captured(4);

  return new Connection(srcElement, srcPad, dstElement, dstPad);
}
