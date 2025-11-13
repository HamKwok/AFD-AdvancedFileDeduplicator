#include <iostream>
#include <filesystem>
#include <fstream>
#include <vector>
#include <map>
#include <string>
#include <algorithm>
#include <chrono>
#include <limits>
#include <sstream>
#include <set>
#include <cstring>
#include <locale>
#include <windows.h>

namespace fs = std::filesystem;

class InteractiveFileDeduplicator {
private:
    bool dryRun;
    bool verbose;
    bool autoConfirm;
    bool skipEmptyFolders;
    size_t samplePoints;
    size_t sampleSize;
    std::string mode;

    struct DeduplicationResult {
    std::vector<std::vector<fs::path>> duplicateGroups;
    int totalFiles = 0;
    uintmax_t totalSize = 0;
    std::string error;
    };

public:
    InteractiveFileDeduplicator(bool dry = false, bool verb = false, bool autoConfirm = false,
                               bool skipEmpty = true, size_t points = 4, size_t size = 4096,
                               const std::string& mod = "all")
        : dryRun(dry), verbose(verb), autoConfirm(autoConfirm), skipEmptyFolders(skipEmpty),
          samplePoints(points), sampleSize(size), mode(mod) {}

    // 获取文件大小
    uintmax_t getFileSize(const fs::path& filepath) {
        return fs::file_size(filepath);
    }

    // 格式化文件大小
    std::string formatFileSize(uintmax_t size) {
        const char* units[] = {"B", "KB", "MB", "GB"};
        int unitIndex = 0;
        double sizeValue = static_cast<double>(size);
        
        while (sizeValue >= 1024.0 && unitIndex < 3) {
            sizeValue /= 1024.0;
            unitIndex++;
        }
        
        char buffer[32];
        snprintf(buffer, sizeof(buffer), "%.2f %s", sizeValue, units[unitIndex]);
        return buffer;
    }

    // 获取文件修改时间
    std::string getFileTimeString(const fs::path& filepath) {
        auto writeTime = fs::last_write_time(filepath);
        auto sctp = std::chrono::time_point_cast<std::chrono::system_clock::duration>(
            writeTime - fs::file_time_type::clock::now() + std::chrono::system_clock::now());
        std::time_t time = std::chrono::system_clock::to_time_t(sctp);
        
        char buffer[64];
        std::strftime(buffer, sizeof(buffer), "%Y-%m-%d %H:%M:%S", std::localtime(&time));
        return buffer;
    }

    // 快速抽样比较
    std::string generateFileSignature(const fs::path& filepath) {
        uintmax_t size = getFileSize(filepath);
        std::string signature = std::to_string(size) + "|";
        
        if (size <= sampleSize * 2) {
            return signature + "SMALL";
        }

        std::ifstream file(filepath, std::ios::binary);
        if (!file) {
            throw std::runtime_error("无法打开文件: " + filepath.string());
        }

        std::vector<uintmax_t> keyPositions = {0};
        
        for (size_t i = 1; i <= samplePoints; ++i) {
            uintmax_t pos = (size * i) / (samplePoints + 1);
            keyPositions.push_back(pos);
        }
        
        keyPositions.push_back(size - std::min(sampleSize, size));

        std::sort(keyPositions.begin(), keyPositions.end());
        keyPositions.erase(std::unique(keyPositions.begin(), keyPositions.end()), keyPositions.end());

        std::vector<char> buffer(sampleSize);
        
        for (uintmax_t pos : keyPositions) {
            uintmax_t readSize = std::min(sampleSize, size - pos);
            file.seekg(pos);
            file.read(buffer.data(), readSize);
            
            if (file.gcount() != readSize) {
                throw std::runtime_error("读取文件失败: " + filepath.string());
            }
            
            uint32_t simpleHash = 0;
            for (size_t i = 0; i < readSize; ++i) {
                simpleHash = (simpleHash * 31) + static_cast<unsigned char>(buffer[i]);
            }
            
            signature += std::to_string(simpleHash) + "|";
        }

        return signature;
    }

    // 逐字节比较文件内容
    bool areFilesIdentical(const fs::path& file1, const fs::path& file2) {
        uintmax_t size1 = getFileSize(file1);
        uintmax_t size2 = getFileSize(file2);
        
        if (size1 != size2) {
            return false;
        }
        
        if (size1 == 0) {
            return true;
        }

        std::ifstream f1(file1, std::ios::binary);
        std::ifstream f2(file2, std::ios::binary);
        
        if (!f1 || !f2) {
            return false;
        }

        const size_t bufferSize = 1024 * 64;
        std::vector<char> buffer1(bufferSize);
        std::vector<char> buffer2(bufferSize);
        
        uintmax_t totalRead = 0;
        
        while (totalRead < size1) {
            size_t toRead = std::min(bufferSize, static_cast<size_t>(size1 - totalRead));
            
            f1.read(buffer1.data(), toRead);
            f2.read(buffer2.data(), toRead);
            
            size_t bytesRead1 = f1.gcount();
            size_t bytesRead2 = f2.gcount();
            
            if (bytesRead1 != bytesRead2 || bytesRead1 != toRead) {
                return false;
            }
            
            if (memcmp(buffer1.data(), buffer2.data(), toRead) != 0) {
                return false;
            }
            
            totalRead += toRead;
        }

        return true;
    }

    // 用户确认函数
    bool askForConfirmation(const std::string& question, bool defaultYes = false) {
        if (autoConfirm) {
            std::cout << question << " (自动确认: 是)" << std::endl;
            return true;
        }
        
        std::cout << question << " [" << (defaultYes ? "Y/n" : "y/N") << "]: ";
        std::cout.flush();
        
        std::string response;
        std::getline(std::cin, response);
        
        if (response.empty()) {
            return defaultYes;
        }
        
        char firstChar = std::tolower(response[0]);
        return (firstChar == 'y');
    }

    // 显示重复文件组（带编号）
    void displayDuplicateGroupsWithNumbers(const std::vector<std::vector<fs::path>>& duplicateGroups) {
        std::cout << "\n" << std::string(80, '=') << std::endl;
        std::cout << "重复文件详细列表 (带编号)" << std::endl;
        std::cout << std::string(80, '=') << std::endl;
        
        for (size_t groupIndex = 0; groupIndex < duplicateGroups.size(); ++groupIndex) {
            const auto& group = duplicateGroups[groupIndex];
            uintmax_t groupSize = getFileSize(group[0]);
            
            std::cout << "\n第 " << (groupIndex + 1) << " 组重复文件 (" << group.size() << " 个文件, " 
                      << formatFileSize(groupSize) << "):" << std::endl;
            std::cout << std::string(60, '-') << std::endl;
            
            for (size_t i = 0; i < group.size(); ++i) {
                std::cout << "  [" << (i + 1) << "] " 
                          << (i == 0 ? "✓ 保留: " : "✗ 删除: ") 
                          << group[i].filename() << std::endl;
                std::cout << "      路径: " << group[i].parent_path() << std::endl;
                std::cout << "      大小: " << formatFileSize(getFileSize(group[i])) 
                          << ", 修改时间: " << getFileTimeString(group[i]) << std::endl;
            }
        }
        
        std::cout << std::string(80, '=') << std::endl;
    }

    // 显示单个重复文件组的详细信息
    void displaySingleGroup(const std::vector<fs::path>& group, int groupIndex) {
        std::cout << "\n" << std::string(60, '=') << std::endl;
        std::cout << "第 " << groupIndex << " 组重复文件 (" << group.size() << " 个文件):" << std::endl;
        std::cout << std::string(60, '=') << std::endl;
        
        for (size_t i = 0; i < group.size(); ++i) {
            std::cout << "  [" << (i + 1) << "] " << group[i].filename() << std::endl;
            std::cout << "      路径: " << group[i] << std::endl;
            std::cout << "      大小: " << formatFileSize(getFileSize(group[i])) 
                      << ", 修改时间: " << getFileTimeString(group[i]) << std::endl;
        }
        std::cout << std::string(60, '=') << std::endl;
    }

    // 显示修改后的保留方案
    void displayModifiedRetention(const std::vector<std::vector<fs::path>>& duplicateGroups,
                                 const std::vector<std::set<size_t>>& keepFiles) {
        std::cout << "\n" << std::string(80, '=') << std::endl;
        std::cout << "修改后的保留方案" << std::endl;
        std::cout << std::string(80, '=') << std::endl;
        
        int totalKept = 0;
        int totalDeleted = 0;
        uintmax_t totalSpaceSaved = 0;

        for (size_t groupIndex = 0; groupIndex < duplicateGroups.size(); ++groupIndex) {
            const auto& group = duplicateGroups[groupIndex];
            const auto& keepSet = keepFiles[groupIndex];
            
            std::cout << "\n第 " << (groupIndex + 1) << " 组重复文件:" << std::endl;
            std::cout << std::string(60, '-') << std::endl;
            
            for (size_t i = 0; i < group.size(); ++i) {
                bool willKeep = (keepSet.find(i + 1) != keepSet.end());
                std::cout << "  " << (willKeep ? "✓ 保留" : "✗ 删除") 
                          << " [" << (i + 1) << "] " << group[i].filename() << std::endl;
                
                if (!willKeep) {
                    totalDeleted++;
                    totalSpaceSaved += getFileSize(group[i]);
                } else {
                    totalKept++;
                }
            }
            
            std::cout << "  本组保留: " << keepSet.size() << " 个文件" << std::endl;
        }
        
        std::cout << std::string(80, '=') << std::endl;
        std::cout << "总计: 保留 " << totalKept << " 个文件, 删除 " << totalDeleted 
                  << " 个文件, 节省 " << formatFileSize(totalSpaceSaved) << std::endl;
        std::cout << std::string(80, '=') << std::endl;
    }

    // 自动选择保留文件的策略
    std::set<size_t> autoSelectKeepFiles(const std::vector<fs::path>& group, const std::string& strategy) {
        std::set<size_t> keepSet;
        
        if (strategy == "newest") {
            // 保留修改时间最新的文件
            std::vector<std::pair<fs::file_time_type, size_t>> times;
            for (size_t i = 0; i < group.size(); ++i) {
                times.emplace_back(fs::last_write_time(group[i]), i + 1);
            }
            std::sort(times.begin(), times.end(), std::greater<>());
            keepSet.insert(times[0].second);
            
        } else if (strategy == "oldest") {
            // 保留修改时间最旧的文件
            std::vector<std::pair<fs::file_time_type, size_t>> times;
            for (size_t i = 0; i < group.size(); ++i) {
                times.emplace_back(fs::last_write_time(group[i]), i + 1);
            }
            std::sort(times.begin(), times.end());
            keepSet.insert(times[0].second);
            
        } else if (strategy == "longest-name") {
            // 保留文件名最长的文件
            size_t maxLength = 0;
            size_t keepIndex = 1;
            for (size_t i = 0; i < group.size(); ++i) {
                size_t length = group[i].filename().string().length();
                if (length > maxLength) {
                    maxLength = length;
                    keepIndex = i + 1;
                }
            }
            keepSet.insert(keepIndex);
            
        } else if (strategy == "shortest-name") {
            // 保留文件名最短的文件
            size_t minLength = std::numeric_limits<size_t>::max();
            size_t keepIndex = 1;
            for (size_t i = 0; i < group.size(); ++i) {
                size_t length = group[i].filename().string().length();
                if (length < minLength) {
                    minLength = length;
                    keepIndex = i + 1;
                }
            }
            keepSet.insert(keepIndex);
        }
        
        return keepSet;
    }

    // 让用户修改保留方案 - 新版本
    std::vector<std::set<size_t>> letUserModifyRetention(const std::vector<std::vector<fs::path>>& duplicateGroups) {
        std::vector<std::set<size_t>> keepFiles(duplicateGroups.size());
        
        // 初始化默认保留方案（每个组保留第一个文件）
        for (size_t i = 0; i < duplicateGroups.size(); ++i) {
            keepFiles[i] = {1};
        }

        std::cout << "\n🛠️  自定义保留方案" << std::endl;
        std::cout << "操作说明:" << std::endl;
        std::cout << "  - 输入组号 (如: 1) 查看并修改该组的保留文件" << std::endl;
        std::cout << "  - 输入 'all' 对所有组使用自动选择" << std::endl;
        std::cout << "  - 输入 'auto' 对当前组使用自动选择" << std::endl;
        std::cout << "  - 输入 'list' 显示所有重复组" << std::endl;
        std::cout << "  - 输入 'done' 完成自定义" << std::endl;
        std::cout << "  - 输入 'view 组号' 查看指定组的详细信息" << std::endl;

        while (true) {
            std::cout << "\n请输入命令 (组号/all/auto/list/done/view): ";
            std::cout.flush();
            
            std::string input;
            std::getline(std::cin, input);
            
            if (input.empty()) {
                continue;
            }
            
            // 转换为小写处理命令
            std::string command = input;
            std::transform(command.begin(), command.end(), command.begin(), ::tolower);
            
            if (command == "done") {
                break;
            }
            else if (command == "list") {
                displayDuplicateGroupsWithNumbers(duplicateGroups);
            }
            else if (command == "all") {
                // 对所有组使用自动选择
                std::cout << "请选择自动保留策略:" << std::endl;
                std::cout << "  1. 保留修改时间最新的文件" << std::endl;
                std::cout << "  2. 保留修改时间最旧的文件" << std::endl;
                std::cout << "  3. 保留文件名最长的文件" << std::endl;
                std::cout << "  4. 保留文件名最短的文件" << std::endl;
                std::cout << "请输入选择 (1-4): ";
                
                std::string strategyInput;
                std::getline(std::cin, strategyInput);
                
                std::string strategy;
                if (strategyInput == "1") strategy = "newest";
                else if (strategyInput == "2") strategy = "oldest";
                else if (strategyInput == "3") strategy = "longest-name";
                else if (strategyInput == "4") strategy = "shortest-name";
                else {
                    std::cout << "无效选择，使用默认策略(最新文件)" << std::endl;
                    strategy = "newest";
                }
                
                for (size_t i = 0; i < duplicateGroups.size(); ++i) {
                    keepFiles[i] = autoSelectKeepFiles(duplicateGroups[i], strategy);
                }
                
                std::cout << "已对所有组应用自动选择策略: " << strategy << std::endl;
                displayModifiedRetention(duplicateGroups, keepFiles);
            }
            else if (command.find("view") == 0) {
                // 查看指定组的详细信息
                std::stringstream ss(input);
                std::string cmd;
                int groupNum;
                ss >> cmd >> groupNum;
                
                if (groupNum < 1 || groupNum > duplicateGroups.size()) {
                    std::cout << "错误: 组号 " << groupNum << " 超出范围 (1-" << duplicateGroups.size() << ")" << std::endl;
                } else {
                    displaySingleGroup(duplicateGroups[groupNum - 1], groupNum);
                }
            }
            else if (command == "auto") {
                std::cout << "请先输入要自动选择的组号: ";
                std::string groupInput;
                std::getline(std::cin, groupInput);
                
                try {
                    int groupNum = std::stoi(groupInput);
                    if (groupNum < 1 || groupNum > duplicateGroups.size()) {
                        std::cout << "错误: 组号 " << groupNum << " 超出范围 (1-" << duplicateGroups.size() << ")" << std::endl;
                        continue;
                    }
                    
                    std::cout << "请选择自动保留策略:" << std::endl;
                    std::cout << "  1. 保留修改时间最新的文件" << std::endl;
                    std::cout << "  2. 保留修改时间最旧的文件" << std::endl;
                    std::cout << "  3. 保留文件名最长的文件" << std::endl;
                    std::cout << "  4. 保留文件名最短的文件" << std::endl;
                    std::cout << "请输入选择 (1-4): ";
                    
                    std::string strategyInput;
                    std::getline(std::cin, strategyInput);
                    
                    std::string strategy;
                    if (strategyInput == "1") strategy = "newest";
                    else if (strategyInput == "2") strategy = "oldest";
                    else if (strategyInput == "3") strategy = "longest-name";
                    else if (strategyInput == "4") strategy = "shortest-name";
                    else {
                        std::cout << "无效选择，使用默认策略(最新文件)" << std::endl;
                        strategy = "newest";
                    }
                    
                    keepFiles[groupNum - 1] = autoSelectKeepFiles(duplicateGroups[groupNum - 1], strategy);
                    std::cout << "已对第 " << groupNum << " 组应用自动选择策略: " << strategy << std::endl;
                    
                } catch (const std::exception& e) {
                    std::cout << "错误: 无效的组号 '" << groupInput << "'" << std::endl;
                }
            }
            else {
                // 处理组号输入
                try {
                    int groupNum = std::stoi(input);
                    if (groupNum < 1 || groupNum > duplicateGroups.size()) {
                        std::cout << "错误: 组号 " << groupNum << " 超出范围 (1-" << duplicateGroups.size() << ")" << std::endl;
                        continue;
                    }
                    
                    const auto& group = duplicateGroups[groupNum - 1];
                    displaySingleGroup(group, groupNum);
                    
                    std::cout << "当前保留的文件: ";
                    for (size_t idx : keepFiles[groupNum - 1]) {
                        std::cout << "[" << idx << "] ";
                    }
                    std::cout << std::endl;
                    
                    std::cout << "请输入要保留的文件编号 (多个编号直接输入无间隔数字，如: 123): ";
                    std::string selection;
                    std::getline(std::cin, selection);
                    
                    std::set<size_t> newKeepSet;
                    bool validInput = true;
                    
                    for (char c : selection) {
                        if (c < '1' || c > '9') {
                            std::cout << "错误: 包含无效字符 '" << c << "'" << std::endl;
                            validInput = false;
                            break;
                        }
                        
                        int fileNum = c - '0';
                        if (fileNum < 1 || fileNum > group.size()) {
                            std::cout << "错误: 文件编号 " << fileNum << " 超出范围 (1-" << group.size() << ")" << std::endl;
                            validInput = false;
                            break;
                        }
                        
                        newKeepSet.insert(fileNum);
                    }
                    
                    if (validInput && !newKeepSet.empty()) {
                        keepFiles[groupNum - 1] = newKeepSet;
                        std::cout << "第 " << groupNum << " 组保留方案已更新: ";
                        for (size_t idx : newKeepSet) {
                            std::cout << "[" << idx << "] ";
                        }
                        std::cout << std::endl;
                    } else if (newKeepSet.empty()) {
                        std::cout << "错误: 至少需要保留一个文件" << std::endl;
                    }
                    
                } catch (const std::exception& e) {
                    std::cout << "错误: 无效输入 '" << input << "'" << std::endl;
                }
            }
        }
        
        return keepFiles;
    }

    // 对候选组进行精确比较
    std::vector<std::vector<fs::path>> findExactDuplicates(const std::vector<fs::path>& candidateGroup) {
        std::vector<std::vector<fs::path>> duplicateGroups;
        std::vector<bool> processed(candidateGroup.size(), false);

        if (verbose) {
            std::cout << "  精确比较 " << candidateGroup.size() << " 个候选文件" << std::endl;
        }

        for (size_t i = 0; i < candidateGroup.size(); ++i) {
            if (processed[i]) continue;

            std::vector<fs::path> duplicateGroup;
            duplicateGroup.push_back(candidateGroup[i]);
            processed[i] = true;

            for (size_t j = i + 1; j < candidateGroup.size(); ++j) {
                if (processed[j]) continue;

                try {
                    if (areFilesIdentical(candidateGroup[i], candidateGroup[j])) {
                        duplicateGroup.push_back(candidateGroup[j]);
                        processed[j] = true;
                    }
                } catch (const std::exception& e) {
                    std::cerr << "比较文件失败: " << candidateGroup[j] << " - " << e.what() << std::endl;
                }
            }

            if (duplicateGroup.size() > 1) {
                duplicateGroups.push_back(duplicateGroup);
            }
        }

        return duplicateGroups;
    }

    // 在单个文件夹内查找重复文件
    DeduplicationResult findDuplicatesInFolder(const fs::path& folder) {
        DeduplicationResult result;
        
        if (!fs::exists(folder) || !fs::is_directory(folder)) {
            result.error = "目录不存在或不是有效目录";
            return result;
        }

        // 第一层：按文件大小分组
        std::map<uintmax_t, std::vector<fs::path>> sizeGroups;
        
        for (const auto& entry : fs::directory_iterator(folder)) {
            if (entry.is_regular_file()) {
                try {
                    uintmax_t size = getFileSize(entry.path());
                    sizeGroups[size].push_back(entry.path());
                    result.totalFiles++;
                    result.totalSize += size;
                } catch (const std::exception& e) {
                    std::cerr << "处理文件出错: " << entry.path() << " - " << e.what() << std::endl;
                }
            }
        }

        // 第二层：抽样比较
        std::map<std::string, std::vector<fs::path>> signatureGroups;

        for (const auto& sizeGroup : sizeGroups) {
            if (sizeGroup.second.size() > 1) {
                for (const auto& filepath : sizeGroup.second) {
                    try {
                        std::string signature = generateFileSignature(filepath);
                        signatureGroups[signature].push_back(filepath);
                    } catch (const std::exception& e) {
                        std::cerr << "生成签名失败: " << filepath << " - " << e.what() << std::endl;
                    }
                }
            }
        }

        // 第三层：逐字节比较
        for (const auto& signatureGroup : signatureGroups) {
            if (signatureGroup.second.size() > 1) {
                auto duplicateGroups = findExactDuplicates(signatureGroup.second);
                for (const auto& group : duplicateGroups) {
                    result.duplicateGroups.push_back(group);
                }
            }
        }

        return result;
    }

    // 处理单个文件夹（带自定义保留功能）
    bool processSingleFolder(const fs::path& folder, int folderIndex = -1, int totalFolders = -1) {
        std::string prefix = "";
        if (folderIndex >= 0 && totalFolders > 0) {
            prefix = "[" + std::to_string(folderIndex) + "/" + std::to_string(totalFolders) + "] ";
        }
        
        std::cout << "\n" << std::string(60, '-') << std::endl;
        std::cout << prefix << "处理文件夹: " << folder << std::endl;
        std::cout << std::string(60, '-') << std::endl;

        auto startTime = std::chrono::high_resolution_clock::now();
        auto result = findDuplicatesInFolder(folder);
        auto endTime = std::chrono::high_resolution_clock::now();
        auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(endTime - startTime);

        if (!result.error.empty()) {
            std::cerr << "错误: " << result.error << std::endl;
            return false;
        }

        // 显示文件夹统计信息
        std::cout << "文件数: " << result.totalFiles << ", 大小: " << formatFileSize(result.totalSize) 
                  << ", 重复组: " << result.duplicateGroups.size() 
                  << ", 耗时: " << duration.count() << " ms" << std::endl;

        // 计算可删除的文件数和节省空间
        int deletableFiles = 0;
        uintmax_t spaceSavable = 0;
        for (const auto& group : result.duplicateGroups) {
            deletableFiles += group.size() - 1;
            spaceSavable += getFileSize(group[0]) * (group.size() - 1);
        }

        std::cout << "可删除文件: " << deletableFiles << " 个, 可节省空间: " << formatFileSize(spaceSavable) << std::endl;

        // 如果没有重复文件且设置了跳过选项
        if (result.duplicateGroups.empty()) {
            if (skipEmptyFolders) {
                std::cout << "⏭️  跳过无重复文件的文件夹" << std::endl;
                return true;
            } else {
                std::cout << "ℹ️  此文件夹无重复文件" << std::endl;
                return true;
            }
        }

        // 显示带编号的重复文件列表
        displayDuplicateGroupsWithNumbers(result.duplicateGroups);

        // 询问是否自定义保留方案
        bool customizeRetention = askForConfirmation("是否要自定义保留哪些文件?", false);
        std::vector<std::set<size_t>> keepFiles;
        
        if (customizeRetention) {
            keepFiles = letUserModifyRetention(result.duplicateGroups);
            displayModifiedRetention(result.duplicateGroups, keepFiles);
        } else {
            // 使用默认方案（每个组保留第一个文件）
            for (const auto& group : result.duplicateGroups) {
                keepFiles.push_back({1});
            }
        }

        // 询问是否确认删除
        bool confirmDelete = askForConfirmation("是否确认按此方案删除重复文件?", false);
        if (!confirmDelete) {
            std::cout << "❌ 跳过此文件夹的删除操作" << std::endl;
            return true;
        }

        // 执行删除操作
        performDeletionWithCustomRetention(result.duplicateGroups, keepFiles);
        return true;
    }

// 收集所有子文件夹
std::vector<fs::path> collectAllSubfolders(const fs::path& rootFolder) {
    std::cout << "正在收集子文件夹..." << std::endl;
    std::vector<fs::path> folders;
    folders.push_back(rootFolder);  // 包括根目录本身

    try {
        for (const auto& entry : fs::recursive_directory_iterator(rootFolder)) {
            if (entry.is_directory()) {
                folders.push_back(entry.path());
                if (verbose) {
                    std::cout << "找到文件夹: " << entry.path() << std::endl;
                }
            }
        }
    } catch (const std::exception& e) {
        std::cerr << "遍历目录时出错: " << e.what() << std::endl;
    }

    // 按路径长度排序，确保父文件夹在前
    std::sort(folders.begin(), folders.end(), [](const fs::path& a, const fs::path& b) {
        return a.string().length() < b.string().length();
    });

    std::cout << "共找到 " << folders.size() << " 个文件夹" << std::endl;
    return folders;
}

// 主去重处理函数
void deduplicate(const std::string& directory) {
    std::cout << "开始处理目录: " << directory << std::endl;
    
    if (!fs::exists(directory)) {
        std::cerr << "错误: 目录不存在: " << directory << std::endl;
        return;
    }
    
    if (!fs::is_directory(directory)) {
        std::cerr << "错误: 路径不是目录: " << directory << std::endl;
        return;
    }

    std::cout << "🎯 文件去重工具 - 模式: " << (mode == "all" ? "全局去重" : "单文件夹去重") << std::endl;
    std::cout << "目标目录: " << directory << std::endl;
    std::cout << "跳过无重复文件夹: " << (skipEmptyFolders ? "是" : "否") << std::endl;

    if (mode == "per-folder" || mode == "folder") {
        // 单文件夹模式：分别处理每个文件夹
        std::cout << "使用单文件夹模式..." << std::endl;
        auto folders = collectAllSubfolders(directory);
        std::cout << "\n找到 " << folders.size() << " 个文件夹需要处理" << std::endl;

        int processedCount = 0;
        int skippedCount = 0;

        for (size_t i = 0; i < folders.size(); ++i) {
            bool result = processSingleFolder(folders[i], i + 1, folders.size());
            if (result) {
                processedCount++;
            } else {
                skippedCount++;
            }

            // 如果不是自动确认模式，询问是否继续处理下一个文件夹
            if (!autoConfirm && i < folders.size() - 1) {
                bool continueProcessing = askForConfirmation("\n是否继续处理下一个文件夹?", true);
                if (!continueProcessing) {
                    std::cout << "⏹️  用户中止处理" << std::endl;
                    break;
                }
            }
        }

        std::cout << "\n" << std::string(50, '=') << std::endl;
        std::cout << "处理完成!" << std::endl;
        std::cout << "已处理: " << processedCount << " 个文件夹" << std::endl;
        if (skippedCount > 0) {
            std::cout << "已跳过: " << skippedCount << " 个文件夹" << std::endl;
        }
        std::cout << std::string(50, '=') << std::endl;

    } else {
        // 全局模式：在整个目录树中查找重复文件
        std::cout << "使用全局模式..." << std::endl;
        
        // 修改 findDuplicatesInFolder 以支持递归扫描
        auto result = findDuplicatesInFolderRecursive(directory);
        
        if (!result.error.empty()) {
            std::cerr << "错误: " << result.error << std::endl;
            return;
        }

        std::cout << "\n" << std::string(50, '=') << std::endl;
        std::cout << "全局扫描完成!" << std::endl;
        std::cout << std::string(50, '=') << std::endl;
        std::cout << "总文件数: " << result.totalFiles << std::endl;
        std::cout << "总大小: " << formatFileSize(result.totalSize) << std::endl;
        std::cout << "发现重复组: " << result.duplicateGroups.size() << " 组" << std::endl;
        
        int totalDuplicateFiles = 0;
        uintmax_t totalSpaceSaved = 0;
        for (const auto& group : result.duplicateGroups) {
            totalDuplicateFiles += group.size() - 1;
            totalSpaceSaved += getFileSize(group[0]) * (group.size() - 1);
        }
        
        std::cout << "重复文件数: " << totalDuplicateFiles << " 个" << std::endl;
        std::cout << "可节省空间: " << formatFileSize(totalSpaceSaved) << std::endl;

        if (result.duplicateGroups.empty()) {
            std::cout << "\n🎉 恭喜！没有找到重复文件。" << std::endl;
            return;
        }

        // 显示带编号的重复文件列表
        displayDuplicateGroupsWithNumbers(result.duplicateGroups);

        // 询问是否自定义保留方案
        bool customizeRetention = askForConfirmation("\n是否要自定义保留哪些文件?", false);
        std::vector<std::set<size_t>> keepFiles;
        
        if (customizeRetention) {
            keepFiles = letUserModifyRetention(result.duplicateGroups);
            displayModifiedRetention(result.duplicateGroups, keepFiles);
        } else {
            // 使用默认方案（每个组保留第一个文件）
            for (const auto& group : result.duplicateGroups) {
                keepFiles.push_back({1});
            }
        }

        // 询问是否确认删除
        bool confirmDelete = askForConfirmation("\n是否确认按此方案删除所有重复文件? (此操作不可恢复)", false);
        if (!confirmDelete) {
            std::cout << "❌ 操作已取消。" << std::endl;
            return;
        }

        // 执行全局删除
        performGlobalDeletionWithCustomRetention(result.duplicateGroups, keepFiles, totalSpaceSaved);
    }
}

// 添加全局模式的支持方法
DeduplicationResult findDuplicatesInFolderRecursive(const fs::path& folder) {
    DeduplicationResult result;
    
    if (!fs::exists(folder) || !fs::is_directory(folder)) {
        result.error = "目录不存在或不是有效目录";
        return result;
    }

    std::cout << "正在递归扫描目录: " << folder << std::endl;

    // 第一层：按文件大小分组（递归扫描所有文件）
    std::map<uintmax_t, std::vector<fs::path>> sizeGroups;
    
    try {
        for (const auto& entry : fs::recursive_directory_iterator(folder)) {
            if (entry.is_regular_file()) {
                try {
                    uintmax_t size = getFileSize(entry.path());
                    sizeGroups[size].push_back(entry.path());
                    result.totalFiles++;
                    result.totalSize += size;
                    
                    if (verbose && result.totalFiles % 100 == 0) {
                        std::cout << "已扫描 " << result.totalFiles << " 个文件..." << std::endl;
                    }
                } catch (const std::exception& e) {
                    std::cerr << "处理文件出错: " << entry.path() << " - " << e.what() << std::endl;
                }
            }
        }
    } catch (const std::exception& e) {
        std::cerr << "遍历目录时出错: " << e.what() << std::endl;
    }

    std::cout << "扫描完成，共找到 " << result.totalFiles << " 个文件" << std::endl;

    // 第二层：抽样比较
    std::map<std::string, std::vector<fs::path>> signatureGroups;
    int samplingCount = 0;

    std::cout << "正在分析文件内容..." << std::endl;
    for (const auto& sizeGroup : sizeGroups) {
        if (sizeGroup.second.size() > 1) {
            for (const auto& filepath : sizeGroup.second) {
                try {
                    std::string signature = generateFileSignature(filepath);
                    signatureGroups[signature].push_back(filepath);
                    samplingCount++;
                    
                    if (verbose && samplingCount % 50 == 0) {
                        std::cout << "已分析 " << samplingCount << " 个文件..." << std::endl;
                    }
                } catch (const std::exception& e) {
                    std::cerr << "生成签名失败: " << filepath << " - " << e.what() << std::endl;
                }
            }
        }
    }

    // 第三层：逐字节比较
    std::cout << "正在确认重复文件..." << std::endl;
    for (const auto& signatureGroup : signatureGroups) {
        if (signatureGroup.second.size() > 1) {
            auto duplicateGroups = findExactDuplicates(signatureGroup.second);
            for (const auto& group : duplicateGroups) {
                result.duplicateGroups.push_back(group);
            }
        }
    }

    return result;
}

// 全局删除方法（带自定义保留）
void performGlobalDeletionWithCustomRetention(const std::vector<std::vector<fs::path>>& duplicateGroups,
                                             const std::vector<std::set<size_t>>& keepFiles,
                                             uintmax_t totalSpaceSaved) {
    std::cout << "\n开始删除重复文件..." << std::endl;
    
    int successfullyDeleted = 0;
    int failedToDelete = 0;
    uintmax_t actualSpaceSaved = 0;

    for (size_t groupIndex = 0; groupIndex < duplicateGroups.size(); ++groupIndex) {
        const auto& group = duplicateGroups[groupIndex];
        const auto& keepSet = keepFiles[groupIndex];
        
        for (size_t i = 0; i < group.size(); ++i) {
            // 如果文件不在保留列表中，则删除
            if (keepSet.find(i + 1) == keepSet.end()) {
                uintmax_t fileSize = getFileSize(group[i]);
                try {
                    if (!dryRun) {
                        fs::remove(group[i]);
                        std::cout << "✓ 已删除: [" << (i + 1) << "] " << group[i].filename() << std::endl;
                    } else {
                        std::cout << "✓ [模拟] 将删除: [" << (i + 1) << "] " << group[i].filename() << std::endl;
                    }
                    successfullyDeleted++;
                    actualSpaceSaved += fileSize;
                } catch (const std::exception& e) {
                    std::cerr << "✗ 删除失败: [" << (i + 1) << "] " << group[i] << " - " << e.what() << std::endl;
                    failedToDelete++;
                }
            }
        }
    }

    std::cout << "\n" << std::string(50, '=') << std::endl;
    std::cout << "删除操作完成!" << std::endl;
    std::cout << std::string(50, '=') << std::endl;
    std::cout << "成功删除: " << successfullyDeleted << " 个文件" << std::endl;
    
    if (failedToDelete > 0) {
        std::cout << "删除失败: " << failedToDelete << " 个文件" << std::endl;
    }
    
    std::cout << "实际节省: " << formatFileSize(actualSpaceSaved) << std::endl;
    
    if (dryRun) {
        std::cout << "注意: 这是模拟运行，没有实际删除文件" << std::endl;
    }
}

private:

    // 执行带自定义保留方案的删除
    void performDeletionWithCustomRetention(const std::vector<std::vector<fs::path>>& duplicateGroups,
                                           const std::vector<std::set<size_t>>& keepFiles) {
        std::cout << "\n开始删除重复文件..." << std::endl;
        
        int successfullyDeleted = 0;
        int failedToDelete = 0;
        uintmax_t actualSpaceSaved = 0;

        for (size_t groupIndex = 0; groupIndex < duplicateGroups.size(); ++groupIndex) {
            const auto& group = duplicateGroups[groupIndex];
            const auto& keepSet = keepFiles[groupIndex];
            
            for (size_t i = 0; i < group.size(); ++i) {
                // 如果文件不在保留列表中，则删除
                if (keepSet.find(i + 1) == keepSet.end()) {
                    uintmax_t fileSize = getFileSize(group[i]);
                    try {
                        if (!dryRun) {
                            fs::remove(group[i]);
                            std::cout << "✓ 已删除: [" << (i + 1) << "] " << group[i].filename() << std::endl;
                        } else {
                            std::cout << "✓ [模拟] 将删除: [" << (i + 1) << "] " << group[i].filename() << std::endl;
                        }
                        successfullyDeleted++;
                        actualSpaceSaved += fileSize;
                    } catch (const std::exception& e) {
                        std::cerr << "✗ 删除失败: [" << (i + 1) << "] " << group[i] << " - " << e.what() << std::endl;
                        failedToDelete++;
                    }
                }
            }
        }

        std::cout << "\n" << std::string(50, '=') << std::endl;
        std::cout << "删除操作完成!" << std::endl;
        std::cout << std::string(50, '=') << std::endl;
        std::cout << "成功删除: " << successfullyDeleted << " 个文件" << std::endl;
        
        if (failedToDelete > 0) {
            std::cout << "删除失败: " << failedToDelete << " 个文件" << std::endl;
        }
        
        std::cout << "实际节省: " << formatFileSize(actualSpaceSaved) << std::endl;
        
        if (dryRun) {
            std::cout << "注意: 这是模拟运行，没有实际删除文件" << std::endl;
        }
    }
};

int main(int argc, char* argv[]) {
    // 全面设置编码
    SetConsoleOutputCP(65001);
    SetConsoleCP(65001);
    
    std::cout << "程序启动..." << std::endl;
    
    // 安全的本地化设置方式
    try {
        std::locale::global(std::locale("")); // 使用系统默认本地化
    } catch (const std::exception& e) {
        std::cout << "警告: 无法设置本地化，使用C本地化 (" << e.what() << ")" << std::endl;
        std::locale::global(std::locale("C"));
    }
    
    if (argc < 2) {
        std::cerr << "错误: 请指定目录路径" << std::endl;
        std::cerr << "使用 -h 查看帮助信息" << std::endl;
        return 1;
    }

    // 显示参数信息
    std::cout << "接收到 " << argc << " 个参数:" << std::endl;
    for (int i = 0; i < argc; i++) {
        std::cout << "  参数[" << i << "]: " << argv[i] << std::endl;
    }

    bool dryRun = false;
    bool verbose = false;
    bool autoConfirm = false;
    bool skipEmptyFolders = true;
    size_t samplePoints = 4;
    size_t sampleSize = 4096;
    std::string mode = "all";  // 默认全局模式
    std::string directory;

// 在 main 函数的参数解析部分，修改为：

// 参数解析
for (int i = 1; i < argc; i++) {
    std::string arg = argv[i];
    std::cout << "解析参数: " << arg << std::endl;
    
    if (arg == "-h" || arg == "--help") {
        std::cout << "高级文件去重工具 - 支持子文件夹处理" << std::endl;
        std::cout << "用法: advanced_dedup [选项] <目录路径>" << std::endl;
        std::cout << "选项:" << std::endl;
        std::cout << "  -d, --dry-run         模拟运行，不实际删除" << std::endl;
        std::cout << "  -v, --verbose         详细输出" << std::endl;
        std::cout << "  -y, --yes             自动确认所有操作" << std::endl;
        std::cout << "  -m, --mode MODE       处理模式: all(全局) 或 folder(单文件夹) [默认: all]" << std::endl;
        std::cout << "  -n, --no-skip         不跳过无重复文件的文件夹" << std::endl;
        std::cout << "  -p, --points NUM      设置抽样点数 (默认: 4)" << std::endl;
        std::cout << "  -s, --size SIZE       设置抽样大小 (默认: 4096)" << std::endl;
        std::cout << std::endl;
        std::cout << "模式说明:" << std::endl;
        std::cout << "  all:    在整个目录树中查找重复文件（跨文件夹比较）" << std::endl;
        std::cout << "  folder: 分别在每个文件夹内查找重复文件（不跨文件夹比较）" << std::endl;
        return 0;
    } else if (arg == "-d" || arg == "--dry-run") {
        dryRun = true;
        std::cout << "设置: 模拟运行模式" << std::endl;
    } else if (arg == "-v" || arg == "--verbose") {
        verbose = true;
        std::cout << "设置: 详细输出模式" << std::endl;
    } else if (arg == "-y" || arg == "--yes") {
        autoConfirm = true;
        std::cout << "设置: 自动确认模式" << std::endl;
    } else if (arg == "-m" || arg == "--mode") {
        if (i + 1 < argc) {
            mode = argv[++i];
            if (mode != "all" && mode != "folder") {
                std::cerr << "错误: 模式必须是 'all' 或 'folder'" << std::endl;
                return 1;
            }
            std::cout << "设置: 模式 = " << mode << std::endl;
        } else {
            std::cerr << "错误: -m 参数需要指定模式" << std::endl;
            return 1;
        }
    } else if (arg == "-n" || arg == "--no-skip") {
        skipEmptyFolders = false;
        std::cout << "设置: 不跳过无重复文件的文件夹" << std::endl;
    } else if (arg == "-p" || arg == "--points") {
        if (i + 1 < argc) {
            try {
                samplePoints = std::stoul(argv[++i]);
                std::cout << "设置: 抽样点数 = " << samplePoints << std::endl;
            } catch (const std::exception& e) {
                std::cerr << "错误: 无效的抽样点数 '" << argv[i] << "'" << std::endl;
                return 1;
            }
        } else {
            std::cerr << "错误: -p 参数需要指定数字" << std::endl;
            return 1;
        }
    } else if (arg == "-s" || arg == "--size") {
        if (i + 1 < argc) {
            try {
                sampleSize = std::stoul(argv[++i]);
                std::cout << "设置: 抽样大小 = " << sampleSize << std::endl;
            } catch (const std::exception& e) {
                std::cerr << "错误: 无效的抽样大小 '" << argv[i] << "'" << std::endl;
                return 1;
            }
        } else {
            std::cerr << "错误: -s 参数需要指定数字" << std::endl;
            return 1;
        }
    } else if (arg[0] != '-') {
        // 这是目录路径
        directory = arg;
        std::cout << "设置: 目录路径 = " << directory << std::endl;
    } else {
        std::cerr << "未知选项: " << arg << std::endl;
        return 1;
    }
}

// 在参数解析完成后，添加路径验证
if (directory.empty()) {
    std::cerr << "错误: 未指定目录路径" << std::endl;
    std::cerr << "请使用: main.exe [选项] <目录路径>" << std::endl;
    std::cerr << "使用 -h 查看帮助信息" << std::endl;
    return 1;
}

// 验证目录是否存在
std::cout << "验证目录是否存在..." << std::endl;
if (!fs::exists(directory)) {
    std::cerr << "错误: 目录不存在: " << directory << std::endl;
    return 1;
}

if (!fs::is_directory(directory)) {
    std::cerr << "错误: 路径不是目录: " << directory << std::endl;
    return 1;
}

std::cout << "目录验证通过" << std::endl;

    std::cout << "最终参数:" << std::endl;
    std::cout << "  目录: " << directory << std::endl;
    std::cout << "  模式: " << mode << std::endl;
    std::cout << "  模拟运行: " << (dryRun ? "是" : "否") << std::endl;
    std::cout << "  详细输出: " << (verbose ? "是" : "否") << std::endl;

    try {
        InteractiveFileDeduplicator dedup(dryRun, verbose, autoConfirm, skipEmptyFolders, 
                                      samplePoints, sampleSize, mode);
        std::cout << "开始执行去重操作..." << std::endl;
        dedup.deduplicate(directory);
        std::cout << "去重操作完成" << std::endl;
    } catch (const std::exception& e) {
        std::cerr << "程序出错: " << e.what() << std::endl;
        return 1;
    }

    std::cout << "程序正常结束" << std::endl;
    return 0;
}