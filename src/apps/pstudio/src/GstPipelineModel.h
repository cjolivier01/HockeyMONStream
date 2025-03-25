#ifndef GSTPIPELINEMODEL_H
#define GSTPIPELINEMODEL_H

#include <gst/gst.h>
#include <QtCore/QList>
#include <QtCore/QMap>
#include <QtCore/QObject>
#include <QtCore/QString>

// Forward declarations
class Element;
class Connection;

class GstPipelineModel : public QObject {
  Q_OBJECT

 public:
  explicit GstPipelineModel(QObject* parent = nullptr);
  ~GstPipelineModel();

  // Pipeline loading/saving
  bool loadFromString(const QString& pipelineString);
  QString toString() const;

  // Pipeline building and state management
  bool buildPipeline();
  bool isBuilt() const;
  bool setState(GstState state);
  GstState state() const;
  void clear();

  // Element management
  bool addElement(const QString& factoryName, const QString& name = QString());
  bool removeElement(const QString& name);
  QStringList elements() const;
  QMap<QString, QString> getElementProperties(const QString& elementName) const;
  bool setElementProperty(const QString& elementName, const QString& property, const QVariant& value);

  // Connection management
  bool connectElements(
      const QString& srcElement,
      const QString& srcPad,
      const QString& sinkElement,
      const QString& sinkPad);
  bool disconnectElements(
      const QString& srcElement,
      const QString& srcPad,
      const QString& sinkElement,
      const QString& sinkPad);
  QList<QPair<QString, QString>> connections() const;

  // Pipeline visualization
  QString toDotFormat() const;

  // Pipeline information
  GstClockTime position() const;
  GstClockTime duration() const;
  QString lastError() const;

 signals:
  void stateChanged(GstState state);
  void errorOccurred(const QString& message);
  void pipelineModified();

 private:
  // Changed from void to gboolean to match implementation
  static gboolean onBusMessage(GstBus* bus, GstMessage* message, GstPipelineModel* model);
  void handleBusMessage(GstMessage* message);
  void setLastError(const QString& error);

  GstElement* m_pipeline;
  GstBus* m_bus;
  guint m_busWatchId;
  bool m_isBuilt;
  QString m_lastError;
  QMap<QString, GstElement*> m_elements;
  QList<QPair<QString, QPair<QString, QString>>> m_connections; // src, (sink, pad)
};

#endif // GSTPIPELINEMODEL_H
