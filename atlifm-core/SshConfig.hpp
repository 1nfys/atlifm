#pragma once

#include "Network.hpp"
#include <QString>

namespace Core::Network {

QString configureSsh(const Location& location);
void removeSshConfiguration(const Location& location);
QString rememberedSshKey(const Location& location);
void rememberSshKey(const Location& location);

}
