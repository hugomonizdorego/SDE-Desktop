// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Selviana DE Project and contributors
//
// The PackageKit D-Bus API numbers Merkadu uses (org.freedesktop.PackageKit,
// PackageKit 1.x: pk-enum.h of PackageKit 1.2 to 1.4) and their texts.
#pragma once

#include <cstdint>
#include <string>

namespace sde {
namespace merkadu {
namespace pk {

// PkFilterEnum values; a filter argument is a bit field (1 << value).
enum Filter : unsigned {
  FILTER_NONE = 1,
  FILTER_INSTALLED = 2,
  FILTER_NOT_INSTALLED = 3,
  FILTER_GUI = 6,
  FILTER_NEWEST = 16,
  FILTER_ARCH = 18,
  FILTER_NOT_SOURCE = 21,
  FILTER_APPLICATION = 24,
};
inline uint64_t bit(unsigned value) { return uint64_t(1) << value; }

// PkTransactionFlagEnum values (bit field like filters).
enum TransactionFlag : unsigned { FLAG_ONLY_TRUSTED = 1, FLAG_SIMULATE = 2 };

// PkInfoEnum values of the Package signal.
enum Info : unsigned {
  INFO_INSTALLED = 1,
  INFO_AVAILABLE = 2,
  INFO_LOW = 3,
  INFO_SECURITY = 8,
  INFO_BLOCKED = 9,
  INFO_DOWNLOADING = 10,
  INFO_UPDATING = 11,
  INFO_INSTALLING = 12,
  INFO_REMOVING = 13,
  INFO_CLEANUP = 14,
  INFO_OBSOLETING = 15,
  INFO_FINISHED = 18,
  INFO_REINSTALLING = 19,
  INFO_DOWNGRADING = 20,
  INFO_PREPARING = 21,
  INFO_DECOMPRESSING = 22,
  INFO_INSTALL = 27,  // planned actions of simulations (PackageKit 1.3)
  INFO_REMOVE = 28,
  INFO_OBSOLETE = 29,
  INFO_DOWNGRADE = 30,
};
// True for the infos a simulated removal gives packages that would go.
bool info_removes(unsigned info);
// True for the infos of packages a simulated installation would add.
bool info_installs(unsigned info);

// PkExitEnum values of the Finished signal.
enum Exit : unsigned {
  FINISHED_SUCCESS = 1,
  FINISHED_FAILED = 2,
  FINISHED_CANCELLED = 3,
  FINISHED_KILLED = 6,
};

// PkStatusEnum values of the Status property.
enum Status : unsigned {
  STATUS_WAIT = 1,
  STATUS_SETUP = 2,
  STATUS_RUNNING = 3,
  STATUS_QUERY = 4,
  STATUS_INFO = 5,
  STATUS_REMOVE = 6,
  STATUS_REFRESH_CACHE = 7,
  STATUS_DOWNLOAD = 8,
  STATUS_INSTALL = 9,
  STATUS_UPDATE = 10,
  STATUS_CLEANUP = 11,
  STATUS_DEP_RESOLVE = 13,
  STATUS_SIG_CHECK = 14,
  STATUS_COMMIT = 16,
  STATUS_FINISHED = 18,
  STATUS_CANCEL = 19,
  STATUS_LOADING_CACHE = 27,
  STATUS_WAITING_FOR_LOCK = 30,
  STATUS_WAITING_FOR_AUTH = 31,
};
// "Downloading", "Installing", "Waiting for authentication" ... (translated);
// "" for statuses not worth showing.
std::string status_text(unsigned status);

// PkErrorEnum values of the ErrorCode signal.
enum Error : unsigned {
  ERROR_NO_NETWORK = 2,
  ERROR_PACKAGE_NOT_INSTALLED = 7,
  ERROR_PACKAGE_NOT_FOUND = 8,
  ERROR_PACKAGE_ALREADY_INSTALLED = 9,
  ERROR_PACKAGE_DOWNLOAD_FAILED = 10,
  ERROR_DEP_RESOLUTION_FAILED = 13,
  ERROR_TRANSACTION_CANCELLED = 17,
  ERROR_CANNOT_REMOVE_SYSTEM_PACKAGE = 20,
  ERROR_CANNOT_GET_LOCK = 26,
  ERROR_NO_SPACE_ON_DEVICE = 46,
  ERROR_NOT_AUTHORIZED = 48,
};
// A sentence for the user; |details| (PackageKit's text) is used for codes
// without one of their own.
std::string error_text(unsigned code, const std::string &details);
bool error_is_cancel(unsigned code);

}  // namespace pk
}  // namespace merkadu
}  // namespace sde
