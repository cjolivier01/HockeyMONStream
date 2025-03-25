#ifndef CONNECTION_H
#define CONNECTION_H

#include <QString>

class Connection {
 public:
  Connection(const QString& srcElement, const QString& srcPad, const QString& dstElement, const QString& dstPad);
  ~Connection();

  // Basic properties
  QString sourceElement() const;
  QString sourcePad() const;
  QString destinationElement() const;
  QString destinationPad() const;

  // Comparison
  bool operator==(const Connection& other) const;

  // Serialization
  QString toString() const;
  static Connection* fromString(const QString& str);

 private:
  QString m_srcElement;
  QString m_srcPad;
  QString m_dstElement;
  QString m_dstPad;
};

#endif // CONNECTION_H
