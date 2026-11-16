// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 zagger fork contributors
#pragma once

#include <QCoreApplication>

// Every UI string added by this fork lives in one translation context so the
// fork's own catalogue stays in a single block of each .ts file instead of
// being scattered over a dozen widget classes.
//
// Source text in the code is always English (Qt convention). Translations are
// looked up at call time, so switching QTranslator at runtime immediately
// changes every string obtained through this macro.
//
//   ZAG_TR("Start scrolling")
//   ZAG_TR("Stitched %1 px").arg(px)
//
#define ZAG_TR(text) QCoreApplication::translate("ZaggerUI", text)
