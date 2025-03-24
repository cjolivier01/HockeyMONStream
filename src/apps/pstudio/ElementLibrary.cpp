#include "ElementLibrary.h"

#include <QVBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QGroupBox>
#include <QToolTip>
#include <gst/gst.h>

ElementLibrary::ElementLibrary(QWidget *parent)
    : QWidget(parent)
    , m_searchEdit(new QLineEdit(this))
    , m_elementList(new QListWidget(this))
{
    createLayout();
    loadElements();
    
    // Connect signals
    connect(m_searchEdit, &QLineEdit::textChanged,
            this, &ElementLibrary::onSearchTextChanged);
    connect(m_elementList, &QListWidget::itemDoubleClicked,
            this, &ElementLibrary::onElementDoubleClicked);
}

ElementLibrary::~ElementLibrary()
{
}

void ElementLibrary::onSearchTextChanged(const QString &text)
{
    updateElementList(text);
}

void ElementLibrary::onElementDoubleClicked(QListWidgetItem *item)
{
    if (!item)
        return;
        
    QString factoryName = item->data(Qt::UserRole).toString();
    emit elementAdded(factoryName);
}

void ElementLibrary::createLayout()
{
    // Create search box
    QHBoxLayout *searchLayout = new QHBoxLayout();
    searchLayout->addWidget(new QLabel("Search:"));
    searchLayout->addWidget(m_searchEdit);
    
    // Configure element list
    m_elementList->setIconSize(QSize(32, 32));
    m_elementList->setDragEnabled(true);
    m_elementList->setSelectionMode(QAbstractItemView::SingleSelection);
    
    // Create main layout
    QVBoxLayout *mainLayout = new QVBoxLayout(this);
    mainLayout->addLayout(searchLayout);
    mainLayout->addWidget(m_elementList);
    
    setLayout(mainLayout);
}

void ElementLibrary::loadElements()
{
    // Define categories for grouping elements
    QStringList categories = {
        "Source", "Sink", "Filter", "Converter", "Encoder", "Decoder", "Other"
    };
    
    // Create a map to store elements by category
    QMap<QString, QStringList> elementsByCategory;
    
    // Initialize categories
    for (const QString &category : categories) {
        elementsByCategory[category] = QStringList();
    }
    
    // Get all element factories
    GList *factories = gst_element_factory_list_get_elements(
        GST_ELEMENT_FACTORY_TYPE_ANY, GST_RANK_NONE);
    
    for (GList *f = factories; f; f = f->next) {
        GstElementFactory *factory = GST_ELEMENT_FACTORY(f->data);
        
        QString factoryName = gst_element_factory_get_name(factory);
        QString description = gst_element_factory_get_description(factory);
        
        // Store element description
        m_elementDescriptions[factoryName] = description;
        
        // Determine category
        QString category = "Other";
        GstElementFactoryListType type = gst_element_factory_get_metadata_as_uint(
            factory, GST_ELEMENT_METADATA_TYPE);
        
        if (type & GST_ELEMENT_FACTORY_TYPE_SRC) {
            category = "Source";
        } else if (type & GST_ELEMENT_FACTORY_TYPE_SINK) {
            category = "Sink";
        } else if (type & GST_ELEMENT_FACTORY_TYPE_FILTER) {
            category = "Filter";
        } else if (type & GST_ELEMENT_FACTORY_TYPE_CONVERTER) {
            category = "Converter";
        } else if (type & GST_ELEMENT_FACTORY_TYPE_ENCODER) {
            category = "Encoder";
        } else if (type & GST_ELEMENT_FACTORY_TYPE_DECODER) {
            category = "Decoder";
        }
        
        // Add element to its category
        elementsByCategory[category].append(factoryName);
    }
    
    // Free the list of factories
    gst_plugin_feature_list_free(factories);
    
    // Create default icons for each category
    m_elementIcons["Source"] = QIcon::fromTheme("media-record");
    m_elementIcons["Sink"] = QIcon::fromTheme("media-playback-stop");
    m_elementIcons["Filter"] = QIcon::fromTheme("view-filter");
    m_elementIcons["Converter"] = QIcon::fromTheme("edit-copy");
    m_elementIcons["Encoder"] = QIcon::fromTheme("document-save");
    m_elementIcons["Decoder"] = QIcon::fromTheme("document-open");
    m_elementIcons["Other"] = QIcon::fromTheme("dialog-question");
    
    // If a theme icon is not available, use a default icon
    for (auto it = m_elementIcons.begin(); it != m_elementIcons.end(); ++it) {
        if (it.value().isNull()) {
            it.value() = QIcon::fromTheme("application-x-executable");
        }
    }
    
    // Update the element list
    updateElementList();
}

void ElementLibrary::updateElementList(const QString &filter)
{
    m_elementList->clear();
    
    // Get all element factories
    GList *factories = gst_element_factory_list_get_elements(
        GST_ELEMENT_FACTORY_TYPE_ANY, GST_RANK_NONE);
    
    for (GList *f = factories; f; f = f->next) {
        GstElementFactory *factory = GST_ELEMENT_FACTORY(f->data);
        
        QString factoryName = gst_element_factory_get_name(factory);
        QString description = gst_element_factory_get_description(factory);
        
        // Apply filter if provided
        if (!filter.isEmpty() && 
            !factoryName.contains(filter, Qt::CaseInsensitive) && 
            !description.contains(filter, Qt::CaseInsensitive)) {
            continue;
        }
        
        // Determine category
        QString category = "Other";
        GstElementFactoryListType type = gst_element_factory_get_metadata_as_uint(
            factory, GST_ELEMENT_METADATA_TYPE);
        
        if (type & GST_ELEMENT_FACTORY_TYPE_SRC) {
            category = "Source";
        } else if (type & GST_ELEMENT_FACTORY_TYPE_SINK) {
            category = "Sink";
        } else if (type & GST_ELEMENT_FACTORY_TYPE_FILTER) {
            category = "Filter";
        } else if (type & GST_ELEMENT_FACTORY_TYPE_CONVERTER) {
            category = "Converter";
        } else if (type & GST_ELEMENT_FACTORY_TYPE_ENCODER) {
            category = "Encoder";
        } else if (type & GST_ELEMENT_FACTORY_TYPE_DECODER) {
            category = "Decoder";
        }
        
        // Create list item
        QListWidgetItem *item = new QListWidgetItem(factoryName);
        item->setToolTip(description);
        item->setData(Qt::UserRole, factoryName);
        item->setIcon(m_elementIcons.value(category, QIcon()));
        
        // Add item to the list
        m_elementList->addItem(item);
    }
    
    // Free the list of factories
    gst_plugin_feature_list_free(factories);
    
    // Sort the list
    m_elementList->sortItems();
}
