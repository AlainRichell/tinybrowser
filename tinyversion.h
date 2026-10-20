// Version and product names of the executable. The resource block in tinybrowser.rc and the setup
// panel read them from here, so a release is one edit and no two places can disagree.
#pragma once

#define TINY_VERSION_MAJOR 1
#define TINY_VERSION_MINOR 0
#define TINY_VERSION_PATCH 0
#define TINY_VERSION_BUILD 0

#define TINY_VERSION_STRING "1.0.0"

#define TINY_PRODUCT_NAME "Tiny Browser"
#define TINY_VENDOR_NAME "Unresolute"

// What a .to says in Explorer's Type column: a snapshot of one window, not the browser itself.
#define TINY_TYPE_NAME "Tiny Browser snapshot"

// The same names for the code: the resource wants narrow text, C++ builds wide strings.
#define TINY_VERSION_WIDE L"1.0.0"
#define TINY_PRODUCT_WIDE L"Tiny Browser"
#define TINY_VENDOR_WIDE L"Unresolute"
#define TINY_TYPE_WIDE L"Tiny Browser snapshot"
