#pragma once

#include <QtCore/QJsonArray>
#include <QtCore/QJsonObject>
#include <filesystem>
#include <string>

namespace hm::web {
struct Request {
  std::filesystem::path root;
  std::filesystem::path game;
  std::string run_id;
  std::string operation;
  QJsonObject payload;
  // Operator-controlled runtime paths, supplied by the worker, never by a browser.
  QJsonObject runtime;
  QJsonArray authorized_media;
};
Request LoadRequest(const std::filesystem::path& path);
std::filesystem::path Contained(const std::filesystem::path& root, const QString& relative, bool must_exist = true);
QJsonObject ReadObject(const std::filesystem::path& path, std::size_t maximum = 16 * 1024 * 1024);
void WriteObject(const std::filesystem::path& path, const QJsonObject& value);
QJsonObject Artifact(const Request& request, const std::filesystem::path& path, const QString& kind);
QString RequiredString(const QJsonObject& value, const QString& key, int maximum = 1024);
double Number(const QJsonObject& value, const QString& key, double minimum, double maximum, double fallback);
std::string Relative(const Request& request, const std::filesystem::path& path);
} // namespace hm::web
