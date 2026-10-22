#pragma once

#include "Network.hpp"
#include <QString>

namespace Core::Network {

bool credentialsAvailable();
QString lookupPassword(const Location& location);
bool savePassword(const Location& location);
void removePassword(const Location& location);

}
