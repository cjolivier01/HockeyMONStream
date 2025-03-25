#include "GstPipelineModel.h"

#include <QDebug>
#include <QRegularExpression>
#include <QStringList>
#include <QVariant>

GstPipelineModel::GstPipelineModel(QObject* parent)
    : QObject(parent), m_pipeline(nullptr), m_bus(nullptr), m_busWatchId(0), m_isBuilt(false) {
  // Create an empty pipeline
  m_pipeline = gst_pipeline_new("pipeline");

  // Get the bus from the pipeline
  m_bus = gst_element_get_bus(m_pipeline);

  // Add a watch for messages
  m_busWatchId = gst_bus_add_watch(m_bus, (GstBusFunc)onBusMessage, this);
}

GstPipelineModel::~GstPipelineModel() {
  // Remove the bus watch
  if (m_busWatchId) {
    g_source_remove(m_busWatchId);
    m_busWatchId = 0;
  }

  // Unreference the bus
  if (m_bus) {
    gst_object_unref(m_bus);
    m_bus = nullptr;
  }

  // Stop and unreference the pipeline
  if (m_pipeline) {
    gst_element_set_state(m_pipeline, GST_STATE_NULL);
    gst_object_unref(m_pipeline);
    m_pipeline = nullptr;
  }
}

bool GstPipelineModel::loadFromString(const QString& pipelineString) {
  // Clear the current pipeline
  clear();

  // Parse the pipeline string
  GError* error = nullptr;
  GstElement* pipeline = gst_parse_launch(pipelineString.toUtf8().constData(), &error);

  if (error) {
    setLastError(QString("Failed to parse pipeline: %1").arg(error->message));
    g_error_free(error);
    return false;
  }

  // Replace our pipeline with the new one
  if (m_pipeline) {
    gst_element_set_state(m_pipeline, GST_STATE_NULL);
    gst_object_unref(m_pipeline);
  }

  m_pipeline = pipeline;

  // Get the bus from the new pipeline
  if (m_bus) {
    gst_object_unref(m_bus);
  }
  m_bus = gst_element_get_bus(m_pipeline);

  // Add a watch for messages
  if (m_busWatchId) {
    g_source_remove(m_busWatchId);
  }
  m_busWatchId = gst_bus_add_watch(m_bus, (GstBusFunc)onBusMessage, this);

  // Extract elements from the pipeline
  m_elements.clear();

  // If it's a bin, iterate through its elements
  if (GST_IS_BIN(m_pipeline)) {
    GstIterator* it = gst_bin_iterate_elements(GST_BIN(m_pipeline));
    GValue item = G_VALUE_INIT;
    bool done = FALSE;

    while (!done) {
      switch (gst_iterator_next(it, &item)) {
        case GST_ITERATOR_OK: {
          GstElement* element = GST_ELEMENT(g_value_get_object(&item));
          QString name = gst_element_get_name(element);
          m_elements[name] = element;
          g_value_reset(&item);
          break;
        }
        case GST_ITERATOR_RESYNC:
          gst_iterator_resync(it);
          break;
        case GST_ITERATOR_ERROR:
          done = TRUE;
          break;
        case GST_ITERATOR_DONE:
          done = TRUE;
          break;
      }
    }

    gst_iterator_free(it);
    g_value_unset(&item);
  }

  // Extract connections
  m_connections.clear();

  // For each element, iterate through its pads and find connections
  for (auto it = m_elements.constBegin(); it != m_elements.constEnd(); ++it) {
    GstElement* element = it.value();
    GstIterator* padIt = gst_element_iterate_pads(element);
    GValue padItem = G_VALUE_INIT;
    bool padDone = FALSE;

    while (!padDone) {
      switch (gst_iterator_next(padIt, &padItem)) {
        case GST_ITERATOR_OK: {
          GstPad* pad = GST_PAD(g_value_get_object(&padItem));
          GstPad* peer = gst_pad_get_peer(pad);

          if (peer) {
            GstElement* peerElement = gst_pad_get_parent_element(peer);

            if (peerElement) {
              QString srcName, sinkName, srcPad, sinkPad;

              // Determine which is source and which is sink
              if (gst_pad_get_direction(pad) == GST_PAD_SRC) {
                srcName = it.key();
                // Get pad name and convert to QString
                gchar* srcPadChars = gst_pad_get_name(pad);
                srcPad = QString(srcPadChars);
                g_free(srcPadChars);

                sinkName = gst_element_get_name(peerElement);
                // Get pad name and convert to QString
                gchar* sinkPadChars = gst_pad_get_name(peer);
                sinkPad = QString(sinkPadChars);
                g_free(sinkPadChars);
              } else {
                sinkName = it.key();
                // Get pad name and convert to QString
                gchar* sinkPadChars = gst_pad_get_name(pad);
                sinkPad = QString(sinkPadChars);
                g_free(sinkPadChars);

                srcName = gst_element_get_name(peerElement);
                // Get pad name and convert to QString
                gchar* srcPadChars = gst_pad_get_name(peer);
                srcPad = QString(srcPadChars);
                g_free(srcPadChars);
              }

              // Add connection
              QPair<QString, QString> sink(sinkName, sinkPad);
              QPair<QString, QPair<QString, QString>> conn(srcName, sink);

              if (!m_connections.contains(conn)) {
                m_connections.append(conn);
              }

              gst_object_unref(peerElement);
            }

            gst_object_unref(peer);
          }

          g_value_reset(&padItem);
          break;
        }
        case GST_ITERATOR_RESYNC:
          gst_iterator_resync(padIt);
          break;
        case GST_ITERATOR_ERROR:
          padDone = TRUE;
          break;
        case GST_ITERATOR_DONE:
          padDone = TRUE;
          break;
      }
    }

    gst_iterator_free(padIt);
    g_value_unset(&padItem);
  }

  m_isBuilt = true;
  emit pipelineModified();

  return true;
}

QString GstPipelineModel::toString() const {
  if (!m_pipeline) {
    return QString();
  }

  // Build a pipeline description from the elements and connections
  QString result;

  // Add elements
  for (auto it = m_elements.constBegin(); it != m_elements.constEnd(); ++it) {
    QString elementName = it.key();
    GstElement* element = it.value();
    GstElementFactory* factory = gst_element_get_factory(element);
    QString factoryName = gst_plugin_feature_get_name(GST_PLUGIN_FEATURE(factory));

    result += QString("%1=%2 ").arg(elementName, factoryName);

    // Add properties
    GParamSpec** properties;
    guint numProps;

    properties = g_object_class_list_properties(G_OBJECT_GET_CLASS(element), &numProps);

    for (guint i = 0; i < numProps; i++) {
      GParamSpec* param = properties[i];

      // Skip properties that can't be read or written
      if (!(param->flags & G_PARAM_READWRITE))
        continue;

      GValue value = G_VALUE_INIT;
      g_value_init(&value, param->value_type);
      g_object_get_property(G_OBJECT(element), param->name, &value);

      // Convert the value to a string
      gchar* strValue = gst_value_serialize(&value);

      if (strValue) {
        result += QString(" %1=%2").arg(param->name, strValue);
        g_free(strValue);
      }

      g_value_unset(&value);
    }

    g_free(properties);

    result += " ! ";
  }

  // Remove the last " ! "
  if (result.endsWith(" ! ")) {
    result.chop(3);
  }

  return result;
}

bool GstPipelineModel::buildPipeline() {
  if (m_isBuilt) {
    // Pipeline is already built
    return true;
  }

  // Stop the current pipeline
  gst_element_set_state(m_pipeline, GST_STATE_NULL);

  // Create a new pipeline
  if (m_pipeline) {
    gst_object_unref(m_pipeline);
  }

  m_pipeline = gst_pipeline_new("pipeline");

  // Get the bus from the new pipeline
  if (m_bus) {
    gst_object_unref(m_bus);
  }
  m_bus = gst_element_get_bus(m_pipeline);

  // Add a watch for messages
  if (m_busWatchId) {
    g_source_remove(m_busWatchId);
  }
  m_busWatchId = gst_bus_add_watch(m_bus, (GstBusFunc)onBusMessage, this);

  // Create all elements and add them to the pipeline
  for (auto it = m_elements.begin(); it != m_elements.end(); ++it) {
    QString elementName = it.key();
    GstElement* oldElement = it.value();
    GstElementFactory* factory = gst_element_get_factory(oldElement);
    QString factoryName = gst_plugin_feature_get_name(GST_PLUGIN_FEATURE(factory));

    GstElement* element = gst_element_factory_make(factoryName.toUtf8().constData(), elementName.toUtf8().constData());

    if (!element) {
      setLastError(QString("Failed to create element: %1").arg(factoryName));
      return false;
    }

    // Get properties from the original element and set them on the new one
    GParamSpec** properties;
    guint numProps;

    properties = g_object_class_list_properties(G_OBJECT_GET_CLASS(it.value()), &numProps);

    for (guint i = 0; i < numProps; i++) {
      GParamSpec* param = properties[i];

      // Skip properties that can't be read or written
      if (!(param->flags & G_PARAM_READWRITE))
        continue;

      GValue value = G_VALUE_INIT;
      g_value_init(&value, param->value_type);
      g_object_get_property(G_OBJECT(it.value()), param->name, &value);

      // Set the property on the new element
      g_object_set_property(G_OBJECT(element), param->name, &value);

      g_value_unset(&value);
    }

    g_free(properties);

    // Add the element to the pipeline
    gst_bin_add(GST_BIN(m_pipeline), element);

    // Update the element in the map
    it.value() = element;
  }

  // Create all connections
  for (const auto& conn : m_connections) {
    QString srcElement = conn.first;
    QString sinkElement = conn.second.first;
    QString sinkPad = conn.second.second;

    GstElement* src = m_elements.value(srcElement);
    GstElement* sink = m_elements.value(sinkElement);

    if (!src || !sink) {
      setLastError(QString("Failed to connect elements: %1 -> %2").arg(srcElement, sinkElement));
      return false;
    }

    if (!gst_element_link(src, sink)) {
      setLastError(QString("Failed to link elements: %1 -> %2").arg(srcElement, sinkElement));
      return false;
    }
  }

  m_isBuilt = true;
  emit pipelineModified();

  return true;
}

bool GstPipelineModel::isBuilt() const {
  return m_isBuilt;
}

bool GstPipelineModel::setState(GstState state) {
  if (!m_pipeline) {
    setLastError("No pipeline to set state on");
    return false;
  }

  GstStateChangeReturn ret = gst_element_set_state(m_pipeline, state);

  if (ret == GST_STATE_CHANGE_FAILURE) {
    setLastError("Failed to change pipeline state");
    return false;
  }

  // If the state change is async, wait for it to complete
  if (ret == GST_STATE_CHANGE_ASYNC) {
    ret = gst_element_get_state(m_pipeline, nullptr, nullptr, GST_CLOCK_TIME_NONE);

    if (ret == GST_STATE_CHANGE_FAILURE) {
      setLastError("Failed to change pipeline state");
      return false;
    }
  }

  emit stateChanged(state);
  return true;
}

GstState GstPipelineModel::state() const {
  if (!m_pipeline) {
    return GST_STATE_NULL;
  }

  GstState state;
  gst_element_get_state(m_pipeline, &state, nullptr, 0);
  return state;
}

void GstPipelineModel::clear() {
  // Stop the pipeline
  if (m_pipeline) {
    gst_element_set_state(m_pipeline, GST_STATE_NULL);
  }

  // Clear elements and connections
  m_elements.clear();
  m_connections.clear();

  m_isBuilt = false;
  emit pipelineModified();
}

bool GstPipelineModel::addElement(const QString& factoryName, const QString& name) {
  // Generate a unique name if not provided
  QString elementName = name;
  if (elementName.isEmpty()) {
    // Extract the base name from the factory name
    QString baseName = factoryName;
    int idx = baseName.indexOf('-');
    if (idx != -1) {
      baseName = baseName.left(idx);
    }

    // Find a unique name
    int counter = 0;
    elementName = baseName + QString::number(counter);
    while (m_elements.contains(elementName)) {
      counter++;
      elementName = baseName + QString::number(counter);
    }
  } else if (m_elements.contains(elementName)) {
    setLastError(QString("Element with name '%1' already exists").arg(elementName));
    return false;
  }

  // Create the element
  GstElement* element = gst_element_factory_make(factoryName.toUtf8().constData(), elementName.toUtf8().constData());

  if (!element) {
    setLastError(QString("Failed to create element: %1").arg(factoryName));
    return false;
  }

  // Set default properties or copy them from the original element

  // Add the element to our map
  m_elements[elementName] = element;

  // If the pipeline is built, add the element to it
  if (m_isBuilt && m_pipeline) {
    gst_bin_add(GST_BIN(m_pipeline), element);
  }

  emit pipelineModified();
  return true;
}

bool GstPipelineModel::removeElement(const QString& name) {
  if (!m_elements.contains(name)) {
    setLastError(QString("Element '%1' not found").arg(name));
    return false;
  }

  // Remove all connections involving this element
  QList<QPair<QString, QPair<QString, QString>>> newConnections;
  for (const auto& conn : m_connections) {
    if (conn.first != name && conn.second.first != name) {
      newConnections.append(conn);
    }
  }
  m_connections = newConnections;

  // Remove the element from the pipeline if it's built
  if (m_isBuilt && m_pipeline) {
    gst_element_set_state(m_elements[name], GST_STATE_NULL);
    gst_bin_remove(GST_BIN(m_pipeline), m_elements[name]);
  } else {
    gst_object_unref(m_elements[name]);
  }

  // Remove the element from our map
  m_elements.remove(name);

  emit pipelineModified();
  return true;
}

QStringList GstPipelineModel::elements() const {
  return m_elements.keys();
}

QMap<QString, QString> GstPipelineModel::getElementProperties(const QString& elementName) const {
  QMap<QString, QString> result;

  if (!m_elements.contains(elementName)) {
    return result;
  }

  GstElement* element = m_elements[elementName];

  // Get all properties
  GParamSpec** properties;
  guint numProps;

  properties = g_object_class_list_properties(G_OBJECT_GET_CLASS(element), &numProps);

  for (guint i = 0; i < numProps; i++) {
    GParamSpec* param = properties[i];

    // Skip properties that can't be read
    if (!(param->flags & G_PARAM_READABLE))
      continue;

    GValue value = G_VALUE_INIT;
    g_value_init(&value, param->value_type);
    g_object_get_property(G_OBJECT(element), param->name, &value);

    // Convert the value to a string
    gchar* strValue = gst_value_serialize(&value);

    if (strValue) {
      result[param->name] = strValue;
      g_free(strValue);
    }

    g_value_unset(&value);
  }

  g_free(properties);

  return result;
}

bool GstPipelineModel::setElementProperty(const QString& elementName, const QString& property, const QVariant& value) {
  if (!m_elements.contains(elementName)) {
    setLastError(QString("Element '%1' not found").arg(elementName));
    return false;
  }

  GstElement* element = m_elements[elementName];

  // Find the property
  GParamSpec* param = g_object_class_find_property(G_OBJECT_GET_CLASS(element), property.toUtf8().constData());

  if (!param) {
    setLastError(QString("Property '%1' not found on element '%2'").arg(property, elementName));
    return false;
  }

  // Skip properties that can't be written
  if (!(param->flags & G_PARAM_WRITABLE)) {
    setLastError(QString("Property '%1' on element '%2' is not writable").arg(property, elementName));
    return false;
  }

  // Convert the QVariant to a GValue
  GValue gvalue = G_VALUE_INIT;
  g_value_init(&gvalue, param->value_type);

  bool success = false;

  // Convert based on the parameter type
  switch (G_TYPE_FUNDAMENTAL(param->value_type)) {
    case G_TYPE_STRING:
      g_value_set_string(&gvalue, value.toString().toUtf8().constData());
      success = true;
      break;

    case G_TYPE_BOOLEAN:
      g_value_set_boolean(&gvalue, value.toBool());
      success = true;
      break;

    case G_TYPE_INT:
      g_value_set_int(&gvalue, value.toInt());
      success = true;
      break;

    case G_TYPE_UINT:
      g_value_set_uint(&gvalue, value.toUInt());
      success = true;
      break;

    case G_TYPE_INT64:
      g_value_set_int64(&gvalue, value.toLongLong());
      success = true;
      break;

    case G_TYPE_UINT64:
      g_value_set_uint64(&gvalue, value.toULongLong());
      success = true;
      break;

    case G_TYPE_FLOAT:
      g_value_set_float(&gvalue, value.toFloat());
      success = true;
      break;

    case G_TYPE_DOUBLE:
      g_value_set_double(&gvalue, value.toDouble());
      success = true;
      break;

    case G_TYPE_ENUM:
      if (value.canConvert<int>()) {
        g_value_set_enum(&gvalue, value.toInt());
        success = true;
      }
      break;

    default:
      // Try to use gst_value_deserialize for complex types
      if (gst_value_deserialize(&gvalue, value.toString().toUtf8().constData())) {
        success = true;
      }
      break;
  }

  if (!success) {
    g_value_unset(&gvalue);
    setLastError(QString("Failed to convert value for property '%1' on element '%2'").arg(property, elementName));
    return false;
  }

  // Set the property
  g_object_set_property(G_OBJECT(element), property.toUtf8().constData(), &gvalue);
  g_value_unset(&gvalue);

  emit pipelineModified();
  return true;
}

bool GstPipelineModel::connectElements(
    const QString& srcElement,
    const QString& srcPad,
    const QString& sinkElement,
    const QString& sinkPad) {
  if (!m_elements.contains(srcElement)) {
    setLastError(QString("Source element '%1' not found").arg(srcElement));
    return false;
  }

  if (!m_elements.contains(sinkElement)) {
    setLastError(QString("Sink element '%1' not found").arg(sinkElement));
    return false;
  }

  // Add the connection to our list
  QPair<QString, QString> sink(sinkElement, sinkPad);
  QPair<QString, QPair<QString, QString>> conn(srcElement, sink);

  if (m_connections.contains(conn)) {
    // Connection already exists
    return true;
  }

  m_connections.append(conn);

  // If the pipeline is built, link the elements
  if (m_isBuilt && m_pipeline) {
    GstElement* src = m_elements[srcElement];
    GstElement* sink = m_elements[sinkElement];

    // Link the elements
    if (!gst_element_link(src, sink)) {
      setLastError(QString("Failed to link elements: %1 -> %2").arg(srcElement, sinkElement));
      return false;
    }
  }

  emit pipelineModified();
  return true;
}

bool GstPipelineModel::disconnectElements(
    const QString& srcElement,
    const QString& srcPad,
    const QString& sinkElement,
    const QString& sinkPad) {
  // Find the connection
  QPair<QString, QString> sink(sinkElement, sinkPad);
  QPair<QString, QPair<QString, QString>> conn(srcElement, sink);

  int index = m_connections.indexOf(conn);

  if (index == -1) {
    setLastError(QString("Connection not found: %1:%2 -> %3:%4").arg(srcElement, srcPad, sinkElement, sinkPad));
    return false;
  }

  // Remove the connection from our list
  m_connections.removeAt(index);

  // If the pipeline is built, unlink the elements
  if (m_isBuilt && m_pipeline) {
    GstElement* src = m_elements[srcElement];
    GstElement* sink = m_elements[sinkElement];

    // Unlink the elements
    gst_element_unlink(src, sink);
  }

  emit pipelineModified();
  return true;
}

QList<QPair<QString, QString>> GstPipelineModel::connections() const {
  QList<QPair<QString, QString>> result;

  for (const auto& conn : m_connections) {
    result.append(qMakePair(conn.first, conn.second.first));
  }

  return result;
}

QString GstPipelineModel::toDotFormat() const {
  if (!m_pipeline) {
    return QString();
  }

  // Generate DOT format for visualization
  gchar* dotString = gst_debug_bin_to_dot_data(GST_BIN(m_pipeline), GST_DEBUG_GRAPH_SHOW_ALL);

  QString result = QString::fromUtf8(dotString);
  g_free(dotString);

  return result;
}

GstClockTime GstPipelineModel::position() const {
  if (!m_pipeline || state() != GST_STATE_PLAYING) {
    return 0;
  }

  GstClockTime position;
  if (!gst_element_query_position(m_pipeline, GST_FORMAT_TIME, (gint64*)&position)) {
    return 0;
  }

  return position;
}

GstClockTime GstPipelineModel::duration() const {
  if (!m_pipeline) {
    return 0;
  }

  GstClockTime duration;
  if (!gst_element_query_duration(m_pipeline, GST_FORMAT_TIME, (gint64*)&duration)) {
    return 0;
  }

  return duration;
}

QString GstPipelineModel::lastError() const {
  return m_lastError;
}

void GstPipelineModel::setLastError(const QString& error) {
  m_lastError = error;
  qDebug() << "GstPipelineModel error:" << error;
}

gboolean GstPipelineModel::onBusMessage(GstBus* bus, GstMessage* message, GstPipelineModel* model) {
  model->handleBusMessage(message);
  return TRUE; // Continue watching
}

void GstPipelineModel::handleBusMessage(GstMessage* message) {
  switch (GST_MESSAGE_TYPE(message)) {
    case GST_MESSAGE_ERROR: {
      GError* err = nullptr;
      gchar* debug = nullptr;

      gst_message_parse_error(message, &err, &debug);

      QString errorMsg = QString("Error from element %1: %2").arg(GST_OBJECT_NAME(message->src), err->message);

      if (debug) {
        errorMsg += QString("\nDebug info: %1").arg(debug);
      }

      g_error_free(err);
      g_free(debug);

      setLastError(errorMsg);
      emit errorOccurred(errorMsg);
      break;
    }

    case GST_MESSAGE_WARNING: {
      GError* err = nullptr;
      gchar* debug = nullptr;

      gst_message_parse_warning(message, &err, &debug);

      QString warningMsg = QString("Warning from element %1: %2").arg(GST_OBJECT_NAME(message->src), err->message);

      if (debug) {
        warningMsg += QString("\nDebug info: %1").arg(debug);
      }

      g_error_free(err);
      g_free(debug);

      qDebug() << warningMsg;
      break;
    }

    case GST_MESSAGE_STATE_CHANGED: {
      // Only handle state changes from the pipeline
      if (GST_MESSAGE_SRC(message) == GST_OBJECT(m_pipeline)) {
        GstState oldState, newState, pendingState;
        gst_message_parse_state_changed(message, &oldState, &newState, &pendingState);

        emit stateChanged(newState);
      }
      break;
    }

    case GST_MESSAGE_EOS:
      // End of stream, stop the pipeline
      gst_element_set_state(m_pipeline, GST_STATE_NULL);
      emit stateChanged(GST_STATE_NULL);
      break;

    default:
      // Ignore other messages
      break;
  }
}
