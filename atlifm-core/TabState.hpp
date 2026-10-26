#pragma once

#include <QString>
#include <filesystem>
#include <vector>
#include <unordered_set>

namespace Core {

struct TabState {
    int id{0};
    QString title;
    std::filesystem::path currentPath;
    std::filesystem::path selectedItem;
    
    std::vector<std::filesystem::path> backHistory;
    std::vector<std::filesystem::path> forwardHistory;
    
    std::unordered_set<std::string> selectedPaths; // для множественного выделения
    int scrollOffset{0};
    int currentRow{0};
    bool isPinned{false};
    bool isSudo{false};
    std::filesystem::path sudoRoot; // каталог, открытый через sudo open (индикатор sudo)
    QString filterQuery;

    void navigateTo(const std::filesystem::path& newPath) {
        std::error_code ec;
        std::filesystem::path canon = std::filesystem::canonical(std::filesystem::absolute(newPath), ec);
        if (ec) canon = newPath;
        if (canon == currentPath) return;
        if (!currentPath.empty()) {
            backHistory.push_back(currentPath);
        }
        forwardHistory.clear();
        currentPath = canon;
        selectedItem.clear();
        selectedPaths.clear();
        currentRow = 0;
        scrollOffset = 0;
        filterQuery.clear();
        isSudo = false;
    }

    bool canGoBack() const {
        return !backHistory.empty();
    }

    bool canGoForward() const {
        return !forwardHistory.empty();
    }

    void goBack() {
        if (!canGoBack()) return;
        forwardHistory.push_back(currentPath);
        currentPath = backHistory.back();
        backHistory.pop_back();
        selectedItem.clear();
        selectedPaths.clear();
    }

    void goForward() {
        if (!canGoForward()) return;
        backHistory.push_back(currentPath);
        currentPath = forwardHistory.back();
        forwardHistory.pop_back();
        selectedItem.clear();
        selectedPaths.clear();
    }
};

}
