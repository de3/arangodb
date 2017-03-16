////////////////////////////////////////////////////////////////////////////////
/// DISCLAIMER
///
/// Copyright 2014-2017 ArangoDB GmbH, Cologne, Germany
/// Copyright 2004-2014 triAGENS GmbH, Cologne, Germany
///
/// Licensed under the Apache License, Version 2.0 (the "License");
/// you may not use this file except in compliance with the License.
/// You may obtain a copy of the License at
///
///     http://www.apache.org/licenses/LICENSE-2.0
///
/// Unless required by applicable law or agreed to in writing, software
/// distributed under the License is distributed on an "AS IS" BASIS,
/// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
/// See the License for the specific language governing permissions and
/// limitations under the License.
///
/// Copyright holder is ArangoDB GmbH, Cologne, Germany
///
/// @author Daniel H. Larkin
////////////////////////////////////////////////////////////////////////////////

#include "Views/LoggerView.h"
#include "Basics/Common.h"
#include "Basics/Result.h"
#include "Logger/Logger.h"
#include "VocBase/LogicalView.h"
#include "VocBase/ViewImplementation.h"
#include "VocBase/voc-types.h"

#include <velocypack/Builder.h>
#include <velocypack/Slice.h>

using namespace arangodb;

static LogLevel LevelStringToEnum(std::string const& level) {
  if (level == "ERR") {
    return LogLevel::ERR;
  }

  if (level == "WARN") {
    return LogLevel::WARN;
  }

  if (level == "INFO") {
    return LogLevel::INFO;
  }

  if (level == "DEBUG") {
    return LogLevel::DEBUG;
  }

  return LogLevel::TRACE;
}

static std::string LevelEnumToString(LogLevel level) {
  switch (level) {
    case LogLevel::ERR:
      return "ERR";
    case LogLevel::WARN:
      return "WARN";
    case LogLevel::INFO:
      return "INFO";
    case LogLevel::DEBUG:
      return "DEBUG";
    default:
      return "TRACE";
  }
}

std::string LoggerView::type("logger");

std::unique_ptr<ViewImplementation> LoggerView::creator(
    LogicalView* view, arangodb::velocypack::Slice const& info) {
  return std::make_unique<LoggerView>(ConstructionGuard(), view, info);
}

LoggerView::LoggerView(ConstructionGuard const& guard, LogicalView* logical,
                       arangodb::velocypack::Slice const& info)
    : ViewImplementation(logical, info) {
  VPackSlice properties = info.get("properties");
  if (!properties.isObject()) {
    _level = LogLevel::TRACE;
    return;
  }

  VPackSlice levelSlice = properties.get("level");
  if (!levelSlice.isString()) {
    _level = LogLevel::TRACE;
    return;
  }

  std::string levelString = levelSlice.copyString();
  _level = LevelStringToEnum(levelString);
}

arangodb::Result LoggerView::updateProperties(
    arangodb::velocypack::Slice const& slice, bool doSync) {
  VPackSlice levelSlice = slice.get("level");
  if (!levelSlice.isString()) {
    return {TRI_ERROR_BAD_PARAMETER,
            "expecting <level> to be specified as string"};
  }

  std::string levelString = levelSlice.copyString();
  _level = LevelStringToEnum(levelString);

  return {};
}

/// @brief export properties
void LoggerView::getPropertiesVPack(velocypack::Builder& builder) const {
  TRI_ASSERT(builder.isOpenObject());
  builder.add("level", VPackValue(LevelEnumToString(_level)));
  TRI_ASSERT(builder.isOpenObject());
}

/// @brief opens an existing view
void LoggerView::open(bool ignoreErrors) {}

void LoggerView::drop() {}
