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

// Helper function to categorize elements based on their name and functionality
QString determineElementCategory(GstElementFactory *factory)
{
    // Default category
    QString category = "Other";
    
    // Get the name and class of the factory
    const gchar *klass = gst_element_factory_get_klass(factory);
    QString klassStr = QString(klass);
    
    // Determine category based on klass string
    if (klassStr.contains("Source") || klassStr.contains("Producer")) {
        category = "Source";
    } else if (klassStr.contains("Sink")) {
        category = "Sink";
    } else if (klassStr.contains("Filter") || klassStr.contains("Effect") || 
              klassStr.contains("Transform")) {
        category = "Filter";
    } else if (klassStr.contains("Codec") || klassStr.contains("Converter") ||
              klassStr.contains("Formatter")) {
        category = "Converter";
    } else if (klassStr.contains("Encoder")) {
        category = "Encoder";
    } else if (klassStr.contains("Decoder")) {
        category = "Decoder";
    }
    
    return category;
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
    GList *factories = gst_registry_get_feature_list(gst_registry_get(), GST_TYPE_ELEMENT_FACTORY);
    
    for (GList *f = factories; f; f = f->next) {
        GstElementFactory *factory = GST_ELEMENT_FACTORY(f->data);
        
        // Get the element factory name using gst_plugin_feature_get_name
        const gchar *name = gst_plugin_feature_get_name(GST_PLUGIN_FEATURE(factory));
        QString factoryName = QString(name);
        
        // Get element description
        const gchar *desc = gst_element_factory_get_metadata(factory, GST_ELEMENT_METADATA_DESCRIPTION);
        QString description = desc ? QString(desc) : "No description available";
        
        // Store element description
        m_elementDescriptions[factoryName] = description;
        
        // Determine category
        QString category = determineElementCategory(factory);
        
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
    GList *factories = gst_registry_get_feature_list(gst_registry_get(), GST_TYPE_ELEMENT_FACTORY);
    
    for (GList *f = factories; f; f = f->next) {
        GstElementFactory *factory = GST_ELEMENT_FACTORY(f->data);
        
        // Get the element factory name using gst_plugin_feature_get_name
        const gchar *name = gst_plugin_feature_get_name(GST_PLUGIN_FEATURE(factory));
        QString factoryName = QString(name);
        
        // Get element description
        const gchar *desc = gst_element_factory_get_metadata(factory, GST_ELEMENT_METADATA_DESCRIPTION);
        QString description = desc ? QString(desc) : "No description available";
        
        // Apply filter if provided
        if (!filter.isEmpty() && 
            !factoryName.contains(filter, Qt::CaseInsensitive) && 
            !description.contains(filter, Qt::CaseInsensitive)) {
            continue;
        }
        
        // Determine category using our helper function
        QString category = determineElementCategory(factory);
        
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
