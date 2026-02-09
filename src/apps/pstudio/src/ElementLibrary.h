#ifndef ELEMENTLIBRARY_H
#define ELEMENTLIBRARY_H

#include <QtWidgets/QLineEdit>
#include <QtGui/QIcon>
#include <QtWidgets/QListWidget>
#include <QtCore/QMap>
#include <QtCore/QString>
#include <QtWidgets/QWidget>

class ElementLibrary : public QWidget {
  Q_OBJECT

 public:
  explicit ElementLibrary(QWidget* parent = nullptr);
  ~ElementLibrary();

 signals:
  void elementAdded(const QString& factoryName);

 private slots:
  void onSearchTextChanged(const QString& text);
  void onElementDoubleClicked(QListWidgetItem* item);

 private:
  void createLayout();
  void loadElements();
  void updateElementList(const QString& filter = QString());

  QLineEdit* m_searchEdit;
  QListWidget* m_elementList;
  QMap<QString, QString> m_elementDescriptions;
  QMap<QString, QIcon> m_elementIcons;
};

#endif // ELEMENTLIBRARY_H
