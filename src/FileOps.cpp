/*
 * FileOps.cpp
 *
 * Implementation of the FileOps class for thread-safe file operations.
 *
 * MIT License
 *
 * Copyright (c) 2025 Swarnendu
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in all
 * copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */
#include "FileOps.hpp"

#include <exception>
#include <tuple>
#include <memory>

// Necessary constants and static variables
static constexpr std::string_view nullString = "";
static constexpr std::string_view DEFAULT_FILE_EXTN = ".txt";
std::vector<std::exception_ptr> FileOps::m_excpPtrVec = {0};

/*static*/ bool FileOps::isFileEmpty(const std::filesystem::path& file) noexcept
{
    if (fileExists(file))
    {
        std::ifstream fileStream(file, std::ios::binary);
        if (fileStream.is_open())
            return fileStream.peek() == std::ifstream::traits_type::eof();
    }
    return false;
}

/*static*/ bool FileOps::fileExists(const std::filesystem::path& file) noexcept
{
    if (file.empty())
        return false;

    return std::filesystem::exists(file);
}

/*static*/ bool FileOps::removeFile(const std::filesystem::path& file) noexcept
{
    if (fileExists(file))
        return std::filesystem::remove(file);
    else
        return false;
}

/*static*/ bool FileOps::clearFile(const std::filesystem::path& file) noexcept
{
    if (fileExists(file))
    {
        std::ofstream outFile(file, std::ios::out | std::ios::trunc);
        if (outFile.is_open())
        {
            outFile.close();
            return true;
        }
    }
    return false;
}

/*static*/ bool FileOps::createFile(const std::filesystem::path& file) noexcept
{
    if (file.empty() || fileExists(file))
        return false;
    
    std::ofstream FILE(file);
    if (FILE.is_open())
    {
        FILE.close();
        return true;
    }
    return false;
}

void FileOps::populateFilePathObj(const StdTupple& fileDetails)
{
    //First of all let us wait for any ongoing file operations (if any) to finish
    std::unique_lock<std::mutex> lock(m_FileOpsMutex);
    m_FileOpsCv.wait(lock, [this] { return !m_isFileOpsRunning; });

    m_isFileOpsRunning = true;  // Make the flag true to indicate that file operations are in progress

    if (!std::get<0>(fileDetails).empty())
        m_FileName = std::get<0>(fileDetails);

    if (!std::get<1>(fileDetails).empty())
        m_FilePath = std::get<1>(fileDetails);

    if (!std::get<2>(fileDetails).empty())
        m_FileExtension = std::get<2>(fileDetails);

    if (!m_FileName.empty())
    {
        if (m_FileExtension.empty())
        {
            if (m_FileName.find('.') != std::string::npos)
            {
                m_FileExtension = m_FileName.substr(m_FileName.find_last_of('.'));
            }
            else
            {
                m_FileExtension = DEFAULT_FILE_EXTN;
                m_FileName += m_FileExtension;
            }
        }
        else
        {
            m_FileName = m_FileName.substr(0, m_FileName.find_last_of('.')); //It is expected that the file name will have only one extension
            m_FileName += m_FileExtension;
        }
        
        auto getSeparator = []()
        {
            #ifdef _WIN32
                return "\\";
            #elif __linux__ || __APPLE__ || __FreeBSD__ || __OpenBSD__ || __NetBSD__ || __MACH__
                return "/";
            #endif
        };

        if (m_FilePath.empty())
        {
            auto pathSeparator = m_FileName.find_last_of('/');
            if (pathSeparator == std::string::npos) //If it is on windows sytem
                pathSeparator = m_FileName.find_last_of('\\');

            if (pathSeparator != std::string::npos)
            {
                //Separate the file name and path from the incoming string
                m_FilePath = m_FileName.substr(0, pathSeparator + 1);
                m_FileName = m_FileName.substr(pathSeparator + 1);
            }
            else    //If the file name does not contain any path, then use the current path
            {
                m_FilePath = std::filesystem::current_path().string();
                m_FilePath += getSeparator();
            }
        }
        else //If the file path doesn't end with a separator, then add it
        {
            if (m_FilePath.back() != '/' && m_FilePath.back() != '\\')
            {
                m_FilePath += getSeparator();
            }
        }
        //Finally create the file path object
        m_FilePathObj = std::filesystem::path(m_FilePath + m_FileName);
    }
    //All done, now we can set the flag to false
    //and notify any waiting threads
    m_isFileOpsRunning = false;
    m_FileOpsCv.notify_one();
}

FileOps::FileOps(const std::uintmax_t maxFileSize,
                 const std::string_view fileName, 
                 const std::string_view filePath, 
                 const std::string_view fileExtension)
    : m_FileName(fileName)
    , m_FilePath(filePath)
    , m_FileExtension(fileExtension)
    , m_DataRecords()
    , m_MaxFileSize(maxFileSize)
{
    auto fileDetails = std::make_tuple(m_FileName, m_FilePath, m_FileExtension);
    // Initialize the file path object
    populateFilePathObj(fileDetails);
    //Spawn a thread to keep watch and pull the data from the data records queue
    //and write it to the file whenever it is available
    std::function<void()> watcherThread = [this]() { this->keepWatchAndPull(); };
    m_watcher = std::thread(std::move(watcherThread));
}

FileOps::~FileOps()
{
    // Wait for any ongoing data operations to finish
    std::unique_lock<std::mutex> dataLock(m_DataRecordsMtx);
    // Set the flag to true to indicate that we are shutting down
    m_shutAndExit = true;
    // Notify the watcher thread to wake up and complete
    // any pending operations before exiting
    dataLock.unlock();
    m_DataRecordsCv.notify_one();

    if (m_watcher.joinable())
        m_watcher.join();
}

FileOps& FileOps::setFileName(const std::string_view fileName)
{
    if (fileName.empty() || fileName == m_FileName)
        return *this;

    populateFilePathObj(std::make_tuple(std::string(fileName), nullString.data(), nullString.data()));
    return *this;   // Builder pattern
}

FileOps& FileOps::setFilePath(const std::string_view filePath)
{
    if (filePath.empty() || filePath == m_FilePath)
        return *this;

    populateFilePathObj(std::make_tuple(nullString.data(), std::string(filePath), nullString.data()));
    return *this;
}

FileOps& FileOps::setFileExtension(const std::string_view fileExtension)
{
    if (fileExtension.empty() || fileExtension == m_FileExtension)
        return *this;

    populateFilePathObj(std::make_tuple(nullString.data(), nullString.data(), std::string(fileExtension)));
    return *this;
}

std::uintmax_t FileOps::getFileSize()
{
    if (fileExists())
    {
        std::scoped_lock<std::mutex> fileLock(m_FileOpsMutex);
        return std::filesystem::file_size(m_FilePathObj);
    }
    return 0;
}

bool FileOps::createFile()
{
    auto retVal = false;
    if (!fileExists())
    {
        std::scoped_lock<std::mutex> lock(m_FileOpsMutex);
        m_isFileOpsRunning = true;
        std::ofstream file(m_FilePathObj);
        if (file.is_open())
        {
            file.close();
            retVal = true;
        }
    }
    m_isFileOpsRunning = false;
    m_FileOpsCv.notify_one();
    return retVal;
}

bool FileOps::deleteFile()
{
    auto retVal = false;
    if (fileExists())
    {
        std::unique_lock<std::mutex> fileLock(m_FileOpsMutex);
        m_FileOpsCv.wait(fileLock, [this] { return !m_isFileOpsRunning; });
        m_isFileOpsRunning = true;
        retVal = std::filesystem::remove(m_FilePathObj);
        m_isFileOpsRunning = false;
        fileLock.unlock();
        m_FileOpsCv.notify_one();
    }
    return retVal;
}

bool FileOps::renameFile(const std::string_view newFileName)
{
    if (newFileName.empty())
        return false;

    auto success = false;

    if (fileExists() && newFileName != m_FileName)
    {
        std::unique_lock<std::mutex> lock(m_FileOpsMutex);
        m_FileOpsCv.wait(lock, [this] { return !m_isFileOpsRunning; });
        m_isFileOpsRunning = true;

        std::filesystem::path newPath = m_FilePathObj.parent_path() / newFileName;
        std::filesystem::rename(m_FilePathObj, newPath);
        m_isFileOpsRunning = false;
        lock.unlock();
        m_FileOpsCv.notify_all();
        success = true;
    }
    return success;
}

void FileOps::readFile()
{
    if (m_FilePathObj.empty())
        throw std::runtime_error("File path is empty");

    // Check if there is any data in the data records queue
    // and wait for it to be processed before reading the file
    while (!m_DataRecords.empty())
    {
        std::unique_lock<std::mutex> dataLock(m_DataRecordsMtx);
        m_DataRecordsCv.wait(dataLock, [this]{ return !m_dataReady; });
        m_dataReady = true;
        dataLock.unlock();
        m_DataRecordsCv.notify_one();
    }

    DataQ().swap(m_FileContent); // Clear the file content queue
    // Wait for any ongoing file operations to finish
    std::this_thread::sleep_for(std::chrono::milliseconds(5));

    std::unique_lock<std::mutex> fileLock(m_FileOpsMutex);
    m_FileOpsCv.wait(fileLock, [this]{ return !m_isFileOpsRunning; });
    m_isFileOpsRunning = true;

    if (std::filesystem::exists(m_FilePathObj))
    {
        std::ifstream file(m_FilePathObj, std::ios::binary);
        if (file.is_open())
        {
            std::string line;
            while (std::getline(file, line))
            {
                m_FileContent.emplace(std::make_shared<std::string>(line.c_str()));
                line.clear();
            }
        }
        else
        {
            m_isFileOpsRunning = false;
            fileLock.unlock();
            m_FileOpsCv.notify_all();
            throw std::runtime_error("Failed to open file: " + m_FilePathObj.string());
        }
    }
    m_isFileOpsRunning = false;
    fileLock.unlock();
    m_FileOpsCv.notify_all();
}

void FileOps::writeFile(const std::string_view data)
{
    if (data.empty())
        return;

    if (!fileExists())
    {
        if (createFile())
            push(data);
        else
            throw std::runtime_error("File neither exists nor can be created");
    }
    else
    {
        push(data);
    }
}

void FileOps::writeFile(const uint8_t data)
{
    writeFile(std::bitset<8>(data).to_string());
}

void FileOps::writeFile(const uint16_t data)
{
    writeFile(std::bitset<16>(data).to_string());
}

void FileOps::writeFile(const uint32_t data)
{
    writeFile(std::bitset<32>(data).to_string());
}

void FileOps::writeFile(const uint64_t data)
{
    writeFile(std::bitset<64>(data).to_string());
}

void FileOps::appendFile(const std::string_view data)
{
    writeFile(data);
}

void FileOps::appendFile(const uint8_t data)
{
    writeFile(std::bitset<8>(data).to_string());
}

void FileOps::appendFile(const uint16_t data)
{
    writeFile(std::bitset<16>(data).to_string());
}

void FileOps::appendFile(const uint32_t data)
{
    writeFile(std::bitset<32>(data).to_string());
}

void FileOps::appendFile(const uint64_t data)
{
    writeFile(std::bitset<64>(data).to_string());
}

void FileOps::appendFile(const std::vector<uint8_t>& binaryStream)
{
    if (!binaryStream.empty())
    {
        for (const auto& bindata : binaryStream)
            writeFile(bindata);
    }
}

void FileOps::appendFile(const std::vector<uint16_t>& binaryStream)
{
    if (!binaryStream.empty())
    {
        for (const auto& bindata : binaryStream)
            writeFile(bindata);
    }
}

void FileOps::appendFile(const std::vector<uint32_t>& binaryStream)
{
    if (!binaryStream.empty())
    {
        for (const auto& bindata : binaryStream)
            writeFile(bindata);
    }
}

void FileOps::appendFile(const std::vector<uint64_t>& binaryStream)
{
    if (!binaryStream.empty())
    {
        for (const auto& bindata : binaryStream)
            writeFile(bindata);
    }
}

void FileOps::writeFile(const std::vector<uint8_t>& binaryStream)
{
    if (!binaryStream.empty())
    {
        for (const auto& bindata : binaryStream)
            writeFile(bindata);
    }
}

void FileOps::writeFile(const std::vector<uint16_t>& binaryStream)
{
    if (!binaryStream.empty())
    {
        for (const auto& bindata : binaryStream)
            writeFile(bindata);
    }
}

void FileOps::writeFile(const std::vector<uint32_t>& binaryStream)
{
    if (!binaryStream.empty())
    {
        for (const auto& bindata : binaryStream)
            writeFile(bindata);
    }
}

void FileOps::writeFile(const std::vector<uint64_t>& binaryStream)
{
    if (!binaryStream.empty())
    {
        for (const auto& bindata : binaryStream)
            writeFile(bindata);
    }
}

bool FileOps::clearFile()
{
    auto retVal = false;
    std::unique_lock<std::mutex> fileLock(m_FileOpsMutex);
    m_FileOpsCv.wait(fileLock, [this]{ return !m_isFileOpsRunning; });
    m_isFileOpsRunning = true;

    if (fileExists())
    {
        std::ofstream file(m_FilePathObj, std::ios::out | std::ios::trunc);
        if (file.is_open())
        {
            file.close();
            retVal = true;
        }
    }
    m_isFileOpsRunning = false;
    fileLock.unlock();
    m_FileOpsCv.notify_all();
    return retVal;
}

void FileOps::push(const std::string_view data)
{
    if (data.empty())
        return;

    auto push = [this](std::array<char, 1025>& dataRecord, const std::string_view data)
    {
        std::copy(data.begin(), data.end(), dataRecord.begin());
        if (data.size() < dataRecord.size())
        {
            auto dataSize = data.size();
            std::fill(dataRecord.begin() + dataSize, dataRecord.end(), '\0');
        }
        m_DataRecords.push(dataRecord);
    };
    
    {
        std::array<char, 1025> dataRecord; // One extra byte for null termination
        std::scoped_lock<std::mutex> lock(m_DataRecordsMtx);
        if (data.size() > dataRecord.size())
        {
            std::string dataCopy = data.data();
            while (dataCopy.size() > dataRecord.size()) // Split the data into 1024 byte chunks
            {
                push(dataRecord, dataCopy.substr(0, dataRecord.size() - 1));
                dataRecord.fill('\0');
                dataCopy = dataCopy.substr(dataRecord.size());
            }
            if (!dataCopy.empty())
            {
                push(dataRecord, dataCopy);
            }
        }
        else
        {
            push(dataRecord, data);
        }
    }
    // If the data queue contains at least 256 elements
    // then notify the watcher thread that data is available
    // and it can start writing to the file
    if (m_DataRecords.size() == 256)
    {
        m_dataReady = true;
        m_DataRecordsCv.notify_one();
    }
}

bool FileOps::pop(BufferQ& data)
{
    if (m_DataRecords.empty())
        return false;

    // Clear the outgoing data buffer
    BufferQ().swap(data);
    data.swap(m_DataRecords);
    m_dataReady = false;

    return true;
}

void FileOps::keepWatchAndPull()
{
    BufferQ dataq;
    // It is an infinite loop, but it will break out of the loop
    // when the m_shutAndExit flag is set to true
    do
    {
        std::unique_lock<std::mutex> dataLock(m_DataRecordsMtx);
        m_DataRecordsCv.wait(dataLock, [this]{ return m_dataReady || m_shutAndExit.load(); });

        auto success = pop(dataq);
        dataLock.unlock();
        m_DataRecordsCv.notify_one();
        // Spawn a thread to write to the file
        // and pass the data queue to it so that
        // the data records queue can be free for
        // other threads to push data to it
        // and the file can be written to in parallel
        // The thread will be joined after the file is written
        std::thread writerThread;
        if (success)
        {
            std::exception_ptr excpPtr = nullptr;
            m_excpPtrVec.emplace_back(excpPtr);
            writerThread = std::thread([this, &dataq, &excpPtr](){ writeToFile(std::move(dataq), excpPtr); });
        }
        if (writerThread.joinable())
            writerThread.join();

        if (m_shutAndExit)
            break;
    } while (true);
}

void FileOps::writeToFile(BufferQ&& dataQueue, std::exception_ptr& excpPtr)
{
    if (dataQueue.empty())
        return;

    try
    {
        std::string errMsg;
        std::unique_lock<std::mutex> fileLock(m_FileOpsMutex);
        m_FileOpsCv.wait(fileLock, [this]{ return !m_isFileOpsRunning; });
        m_isFileOpsRunning = true;

        std::ofstream file(m_FilePathObj, std::ios::out | std::ios::app | std::ios::binary);
        if (file.is_open())
        {
            while (!dataQueue.empty())
            {
                auto data = dataQueue.front();
                dataQueue.pop();
                file << data.data() << "\n";
                file.flush();
            }
            file.close();
        }
        else
        {
            std::ostringstream osstr;
            osstr << "WRITING_ERROR : [";
            osstr << std::this_thread::get_id();
            osstr << "]: File [" << m_FilePathObj << "] can not be opened to write data;";
            errMsg = osstr.str();
        }
        m_isFileOpsRunning = false;
        fileLock.unlock();
        m_FileOpsCv.notify_all();

        if (!errMsg.empty())
            throw std::runtime_error(errMsg);
    }
    catch(...)
    {
        excpPtr = std::current_exception();
    }
}

