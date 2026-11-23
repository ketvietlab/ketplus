#pragma once

#include <QtGlobal>

struct CatalogCallTiming {
    qint64 wallNs{};
    qint64 cpuNs{};
    qsizetype calls{};
};

void beginCatalogCallTiming();
CatalogCallTiming endCatalogCallTiming();
