// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Selviana DE Project and contributors
//
// All Software, Synaptic's way of working (packages.h): the page of every
// package of the repositories with the sections (a column on wide windows,
// a menu on narrow ones), the status filters, the list, the selected
// package with its marks and the bar of the marked changes; the page of one
// package (description, dependencies, versions); and the summary of the
// marked changes before they are applied.
#pragma once

#include <string>

#include "packages.h"

namespace sde {

class Page;

namespace merkadu {

// The argument of View::Packages: "STATUS|SECTION|QUERY" (package_status_id()).
std::string packages_view_arg(const PackageFilter &f);
PackageFilter packages_filter(const std::string &arg);

void build_packages_page(Page *page, const std::string &arg);
void build_package_page(Page *page, const std::string &name);
void build_changes_page(Page *page);
// The bar title of the pages.
std::string packages_title(const std::string &arg);

}  // namespace merkadu
}  // namespace sde
