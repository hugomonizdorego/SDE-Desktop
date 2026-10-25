// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Selviana DE Project and contributors
#include "pkenum.h"

#include "i18n.h"
#include "util.h"

namespace sde {
namespace merkadu {
namespace pk {

bool info_removes(unsigned info) {
  return info == INFO_REMOVING || info == INFO_REMOVE || info == INFO_OBSOLETING || info == INFO_OBSOLETE;
}

bool info_installs(unsigned info) {
  return info == INFO_INSTALLING || info == INFO_INSTALL || info == INFO_AVAILABLE;
}

std::string status_text(unsigned status) {
  switch (status) {
    case STATUS_WAIT: return _("Waiting in the queue");
    case STATUS_SETUP: return _("Starting");
    case STATUS_QUERY:
    case STATUS_INFO:
    case STATUS_DEP_RESOLVE: return _("Resolving dependencies");
    case STATUS_REMOVE: return _("Removing");
    case STATUS_REFRESH_CACHE: return _("Refreshing the package lists");
    case STATUS_DOWNLOAD: return _("Downloading");
    case STATUS_INSTALL: return _("Installing");
    case STATUS_UPDATE: return _("Updating");
    case STATUS_CLEANUP: return _("Cleaning up");
    case STATUS_SIG_CHECK: return _("Checking signatures");
    case STATUS_COMMIT: return _("Applying changes");
    case STATUS_FINISHED: return _("Finished");
    case STATUS_CANCEL: return _("Cancelling");
    case STATUS_LOADING_CACHE: return _("Loading the package cache");
    case STATUS_WAITING_FOR_LOCK: return _("Waiting for another package manager to finish");
    case STATUS_WAITING_FOR_AUTH: return _("Waiting for authentication");
    default: return "";
  }
}

std::string error_text(unsigned code, const std::string &details) {
  switch (code) {
    case ERROR_NO_NETWORK: return _("There is no connection to the internet.");
    case ERROR_PACKAGE_NOT_INSTALLED: return _("The package is not installed.");
    case ERROR_PACKAGE_NOT_FOUND: return _("The package was not found in the package sources.");
    case ERROR_PACKAGE_ALREADY_INSTALLED: return _("The package is already installed.");
    case ERROR_PACKAGE_DOWNLOAD_FAILED: return _("A package could not be downloaded.");
    case ERROR_DEP_RESOLUTION_FAILED:
      return _("The packages it needs cannot be installed together with the installed software.");
    case ERROR_TRANSACTION_CANCELLED: return _("Cancelled.");
    case ERROR_CANNOT_REMOVE_SYSTEM_PACKAGE: return _("It is an essential part of the system.");
    case ERROR_CANNOT_GET_LOCK: return _("Another program is installing or removing software. Try again when it has finished.");
    case ERROR_NO_SPACE_ON_DEVICE: return _("There is not enough free disk space.");
    case ERROR_NOT_AUTHORIZED: return _("You are not allowed to do this, or the authentication failed.");
    default: break;
  }
  std::string d = trim(details);
  return d.empty() ? std::string(_("The package system reported an error.")) : d;
}

bool error_is_cancel(unsigned code) { return code == ERROR_TRANSACTION_CANCELLED; }

}  // namespace pk
}  // namespace merkadu
}  // namespace sde
