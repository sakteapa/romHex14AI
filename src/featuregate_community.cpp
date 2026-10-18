/*
 * This file is part of romHEX14.
 * Copyright (C) 2026 Cristian Tabuyo <contact@romhex14.com>
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "featuregate.h"

namespace FeatureGate {

bool isAvailable(const QString &)
{
    return true;
}

bool isLoggedIn()
{
    return true;
}

QString userEmail()
{
    return QStringLiteral("Community Edition (Offline)");
}

} // namespace FeatureGate
