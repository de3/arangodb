////////////////////////////////////////////////////////////////////////////////
/// DISCLAIMER
///
/// Copyright 2016 ArangoDB GmbH, Cologne, Germany
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
/// @author Jan Steemann
////////////////////////////////////////////////////////////////////////////////

#include "ApplicationServer.h"

#include "ApplicationFeatures/ApplicationFeature.h"
#include "Basics/StringUtils.h"
#include "ProgramOptions/ArgumentParser.h"
#include "Logger/Logger.h"

using namespace arangodb::application_features;
using namespace arangodb::basics;
using namespace arangodb::options;

ApplicationServer* ApplicationServer::server = nullptr;

ApplicationServer::ApplicationServer(std::shared_ptr<ProgramOptions> options)
    : _options(options),
      _stopping(false),
      _privilegesDropped(false),
      _dumpDependencies(false) {
  if (ApplicationServer::server != nullptr) {
    LOG(ERR) << "ApplicationServer initialized twice";
  }

  ApplicationServer::server = this;
}

ApplicationServer::~ApplicationServer() {
  for (auto& it : _features) {
    delete it.second;
  }

  ApplicationServer::server = nullptr;
}

void ApplicationServer::throwFeatureNotFoundException(std::string const& name) {
  THROW_ARANGO_EXCEPTION_MESSAGE(TRI_ERROR_INTERNAL,
                                 "unknown feature '" + name + "'");
}

void ApplicationServer::throwFeatureNotEnabledException(std::string const& name) {
  THROW_ARANGO_EXCEPTION_MESSAGE(TRI_ERROR_INTERNAL,
                                 "feature '" + name + "' is not enabled");
}

ApplicationFeature* ApplicationServer::lookupFeature(std::string const& name) {
  if (ApplicationServer::server == nullptr) {
    return nullptr;
  }

  try {
    return ApplicationServer::server->feature(name);
  } catch (...) {
  }

  return nullptr;
}

void ApplicationServer::disableFeatures(std::vector<std::string> const& names) {
  disableFeatures(names, false);
}

void ApplicationServer::forceDisableFeatures(std::vector<std::string> const& names) {
  disableFeatures(names, true);
}

void ApplicationServer::disableFeatures(std::vector<std::string> const& names, bool force) {
  for (auto const& name : names) {
    auto feature = ApplicationServer::lookupFeature(name);

    if (feature != nullptr) {
      if (force) {
        feature->forceDisable();
      } else {
        feature->disable();
      }
    }
  }
}

// adds a feature to the application server. the application server
// will take ownership of the feature object and destroy it in its
// destructor
void ApplicationServer::addFeature(ApplicationFeature* feature) {
  _features.emplace(feature->name(), feature);
}

// checks for the existence of a named feature. will not throw when used for
// a non-existing feature
bool ApplicationServer::exists(std::string const& name) const {
  return (_features.find(name) != _features.end());
}

// returns a pointer to a named feature. will throw when used for
// a non-existing feature
ApplicationFeature* ApplicationServer::feature(std::string const& name) const {
  auto it = _features.find(name);

  if (it == _features.end()) {
    throwFeatureNotFoundException(name);
  }
  return (*it).second;
}

// return whether or not a feature is enabled
// will throw when called for a non-existing feature
bool ApplicationServer::isEnabled(std::string const& name) const {
  return feature(name)->isEnabled();
}

// return whether or not a feature is optional
// will throw when called for a non-existing feature
bool ApplicationServer::isOptional(std::string const& name) const {
  return feature(name)->isOptional();
}

// return whether or not a feature is required
// will throw when called for a non-existing feature
bool ApplicationServer::isRequired(std::string const& name) const {
  return feature(name)->isRequired();
}

// this method will initialize and validate options
// of all feature, start them and wait for a shutdown
// signal. after that, it will shutdown all features
void ApplicationServer::run(int argc, char* argv[]) {
  LOG_TOPIC(TRACE, Logger::STARTUP) << "ApplicationServer::run";

  // collect options from all features
  // in this phase, all features are order-independent
  collectOptions();

  // setup dependency, but ignore any failure for now
  setupDependencies(false);

  // parse the command line parameters and load any configuration
  // file(s)
  parseOptions(argc, argv);

  // seal the options
  _options->seal();

  // validate options of all features
  validateOptions();

  // enable automatic features
  enableAutomaticFeatures();

  // setup and validate all feature dependencies
  setupDependencies(true);

  // allows process control
  daemonize();

  // now the features will actually do some preparation work
  // in the preparation phase, the features must not start any threads
  // furthermore, they must not write any files under elevated privileges
  // if they want other features to access them, or if they want to access
  // these files with dropped privileges
  prepare();

  // permanently drop the privileges
  dropPrivilegesPermanently();

  // start features. now features are allowed to start threads, write files etc.
  start();

  // wait until we get signaled the shutdown request
  wait();

  // stop all features
  stop();
}

// signal the server to shut down
void ApplicationServer::beginShutdown() {
  LOG_TOPIC(TRACE, Logger::STARTUP) << "ApplicationServer::beginShutdown";

  // fowards the begin shutdown signal to all features
  for (auto it = _orderedFeatures.rbegin(); it != _orderedFeatures.rend();
       ++it) {
    if ((*it)->isEnabled()) {
      LOG_TOPIC(TRACE, Logger::STARTUP) << (*it)->name() << "::beginShutdown";
      (*it)->beginShutdown();
    }
  }

  _stopping = true;
  // TODO: use condition variable for signaling shutdown
  // to run method
}

VPackBuilder ApplicationServer::options(
    std::unordered_set<std::string> const& excludes) const {
  return _options->toVPack(false, excludes);
}

// fail and abort with the specified message
void ApplicationServer::fail(std::string const& message) {
  LOG(FATAL) << "error. cannot proceed. reason: " << message;
  FATAL_ERROR_EXIT();
}

// walks over all features and runs a callback function for them
// the order in which features are visited is unspecified
void ApplicationServer::apply(std::function<void(ApplicationFeature*)> callback,
                              bool enabledOnly) {
  for (auto& it : _features) {
    if (!enabledOnly || it.second->isEnabled()) {
      callback(it.second);
    }
  }
}

void ApplicationServer::collectOptions() {
  LOG_TOPIC(TRACE, Logger::STARTUP) << "ApplicationServer::collectOptions";

  _options->addSection(
      Section("", "Global configuration", "global options", false, false));

  _options->addHiddenOption("--dump-dependencies", "dump dependency graph",
                            new BooleanParameter(&_dumpDependencies, false));

  apply([this](ApplicationFeature* feature) {
    LOG_TOPIC(TRACE, Logger::STARTUP) << feature->name() << "::loadOptions";
    feature->collectOptions(_options);
  }, true);
}

void ApplicationServer::parseOptions(int argc, char* argv[]) {
  ArgumentParser parser(_options.get());

  std::string helpSection = parser.helpSection(argc, argv);

  if (!helpSection.empty()) {
    // user asked for "--help"

    // translate "all" to "*"
    if (helpSection == "all") {
      helpSection = "*";
    }
    _options->printHelp(helpSection);
    exit(EXIT_SUCCESS);
  }

  if (!parser.parse(argc, argv)) {
    // command-line option parsing failed. an error was already printed
    // by now, so we can exit
    exit(EXIT_FAILURE);
  }

  if (_dumpDependencies) {
    std::cout << "digraph dependencies\n"
              << "{\n"
              << "  overlap = false;\n";
    for (auto feature : _features) {
      for (auto before : feature.second->startsAfter()) {
        std::cout << "  " << feature.first << " -> " << before << ";\n";
      }
    }
    std::cout << "}\n";
    exit(EXIT_SUCCESS);
  }

  for (auto it = _orderedFeatures.begin(); it != _orderedFeatures.end(); ++it) {
    if ((*it)->isEnabled()) {
      LOG_TOPIC(TRACE, Logger::STARTUP) << (*it)->name() << "::loadOptions";
      (*it)->loadOptions(_options);
    }
  }
}

void ApplicationServer::validateOptions() {
  LOG_TOPIC(TRACE, Logger::STARTUP) << "ApplicationServer::validateOptions";

  for (auto it = _orderedFeatures.begin(); it != _orderedFeatures.end(); ++it) {
    if ((*it)->isEnabled()) {
      LOG_TOPIC(TRACE, Logger::STARTUP) << (*it)->name() << "::validateOptions";
      (*it)->validateOptions(_options);
    }
  }
}

void ApplicationServer::enableAutomaticFeatures() {
  bool changed;
  do {
    changed = false;
    for (auto& it : _features) {
      auto other = it.second->enableWith();
      if (other.empty()) {
        continue;
      }
      if (!this->exists(other)) {
        fail("feature '" + it.second->name() +
             "' depends on unknown feature '" + other + "'");
      }
      bool otherIsEnabled = this->feature(other)->isEnabled();
      if (otherIsEnabled != it.second->isEnabled()) {
        it.second->setEnabled(otherIsEnabled);
        changed = true;
      }
    }
  } while (changed);
}

// setup and validate all feature dependencies, determine feature order
void ApplicationServer::setupDependencies(bool failOnMissing) {
  LOG_TOPIC(TRACE, Logger::STARTUP)
      << "ApplicationServer::validateDependencies";

  // first check if a feature references an unknown other feature
  if (failOnMissing) {
    apply([this](ApplicationFeature* feature) {
      for (auto& other : feature->requires()) {
        if (!this->exists(other)) {
          fail("feature '" + feature->name() +
               "' depends on unknown feature '" + other + "'");
        }
        if (!this->feature(other)->isEnabled()) {
          fail("enabled feature '" + feature->name() +
               "' depends on other feature '" + other + "', which is disabled");
        }
      }
    }, true);
  }

  // first insert all features, even the inactive ones
  std::vector<ApplicationFeature*> features;
  for (auto& it : _features) {
    auto insertPosition = features.end();

    if (!features.empty()) {
      for (size_t i = features.size(); i > 0; --i) {
        if (it.second->doesStartBefore(features[i - 1]->name())) {
          insertPosition = features.begin() + (i - 1);
        }
      }
    }
    features.insert(insertPosition, it.second);
  }

  LOG_TOPIC(TRACE, Logger::STARTUP) << "ordered features:";

  for (auto feature : features) {
    LOG_TOPIC(TRACE, Logger::STARTUP)
        << "  " << feature->name()
        << (feature->isEnabled() ? "" : "(disabled)");

    auto startsAfter = feature->startsAfter();

    if (!startsAfter.empty()) {
      LOG_TOPIC(TRACE, Logger::STARTUP)
          << "    " << StringUtils::join(feature->startsAfter(), ", ");
    }
  }

  // remove all inactive features
  for (auto it = features.begin(); it != features.end(); /* no hoisting */) {
    if ((*it)->isEnabled()) {
      // keep feature
      ++it;
    } else {
      // remove feature
      it = features.erase(it);
    }
  }

  _orderedFeatures = features;
}

void ApplicationServer::daemonize() {
  LOG_TOPIC(TRACE, Logger::STARTUP) << "ApplicationServer::daemonize";

  for (auto it = _orderedFeatures.begin(); it != _orderedFeatures.end(); ++it) {
    if ((*it)->isEnabled()) {
      (*it)->daemonize();
    }
  }
}

void ApplicationServer::prepare() {
  LOG_TOPIC(TRACE, Logger::STARTUP) << "ApplicationServer::prepare";

  // we start with elevated privileges
  bool privilegesElevated = true;

  for (auto it = _orderedFeatures.begin(); it != _orderedFeatures.end(); ++it) {
    if ((*it)->isEnabled()) {
      bool const requiresElevated = (*it)->requiresElevatedPrivileges();

      if (requiresElevated != privilegesElevated) {
        // must change privileges for the feature
        if (requiresElevated) {
          raisePrivilegesTemporarily();
          privilegesElevated = true;
        } else {
          dropPrivilegesTemporarily();
          privilegesElevated = false;
        }
      }

      try {
        LOG_TOPIC(TRACE, Logger::STARTUP) << (*it)->name() << "::prepare";
        (*it)->prepare();
      } catch (...) {
        // restore original privileges
        if (!privilegesElevated) {
          raisePrivilegesTemporarily();
        }
        throw;
      }
    }
  }
}

void ApplicationServer::start() {
  LOG_TOPIC(TRACE, Logger::STARTUP) << "ApplicationServer::start";

  for (auto it = _orderedFeatures.begin(); it != _orderedFeatures.end(); ++it) {
    LOG_TOPIC(TRACE, Logger::STARTUP) << (*it)->name() << "::start";
    (*it)->start();
  }
}

void ApplicationServer::stop() {
  LOG_TOPIC(TRACE, Logger::STARTUP) << "ApplicationServer::stop";

  for (auto it = _orderedFeatures.rbegin(); it != _orderedFeatures.rend();
       ++it) {
    LOG_TOPIC(TRACE, Logger::STARTUP) << (*it)->name() << "::stop";
    (*it)->stop();
  }
}

void ApplicationServer::wait() {
  LOG_TOPIC(TRACE, Logger::STARTUP) << "ApplicationServer::wait";

  while (!_stopping) {
    // TODO: use condition variable for waiting for shutdown
    ::usleep(100000);
  }
}

// temporarily raise privileges
void ApplicationServer::raisePrivilegesTemporarily() {
  if (_privilegesDropped) {
    THROW_ARANGO_EXCEPTION_MESSAGE(
        TRI_ERROR_INTERNAL, "must not raise privileges after dropping them");
  }
  
  LOG_TOPIC(TRACE, Logger::STARTUP) << "raising privileges";

  // TODO
}

// temporarily drop privileges
void ApplicationServer::dropPrivilegesTemporarily() {
  if (_privilegesDropped) {
    THROW_ARANGO_EXCEPTION_MESSAGE(
        TRI_ERROR_INTERNAL,
        "must not try to drop privileges after dropping them");
  }
  
  LOG_TOPIC(TRACE, Logger::STARTUP) << "dropping privileges";

  // TODO
}

// permanently dropped privileges
void ApplicationServer::dropPrivilegesPermanently() {
  if (_privilegesDropped) {
    THROW_ARANGO_EXCEPTION_MESSAGE(
        TRI_ERROR_INTERNAL,
        "must not try to drop privileges after dropping them");
  }
  _privilegesDropped = true;

  // TODO
}