// Copyright (c) 2014, Baidu.com, Inc. All Rights Reserved
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
//
// Author: yanshiguang02@baidu.com

#include <fcntl.h>
#include "proto/nameserver.pb.h"
#include "proto/chunkserver.pb.h"
#include "rpc/rpc_client.h"
#include "common/mutex.h"
#include "common/timer.h"
#include "bfs.h"

extern std::string FLAGS_nameserver;

namespace bfs {

struct LocatedBlocks {
    int64_t _file_length;
    std::vector<LocatedBlock> _blocks;
    void CopyFrom(const ::google::protobuf::RepeatedPtrField< ::bfs::LocatedBlock >& blocks) {
        for (int i = 0; i < blocks.size(); i++) {
            _blocks.push_back(blocks.Get(i));
        }
    }
};

class FSImpl;

class WriteBuffer {
public:
    WriteBuffer(int buf_size = 1024)
        : _buf_size(buf_size), _data_size(0) {
        _buf = new char[buf_size];
    }
    ~WriteBuffer() {
        delete _buf;
        _buf = NULL;
    }
    int Available() {
        return _buf_size - _data_size;
    }
    int Append(const char* buf, int len) {
        assert(len + _data_size <= _buf_size);
        memcpy(_buf + _data_size, buf, len);
        _data_size += len;
        return _data_size;
    }
    const char* Data() {
        return _buf;
    }
    int Size() {
        return _data_size;
    }
    void Clear() {
        _data_size = 0;
    }
private:
    int _buf_size;
    int _data_size;
    char* _buf;
};

class BfsFileImpl : public File {
public:
    BfsFileImpl(FSImpl* fs, const std::string name, int32_t flags);
    ~BfsFileImpl ();
    int64_t Pread(char* buf, int64_t read_size, int64_t offset);
    int64_t Seek(int64_t offset, int32_t whence);
    int64_t Read(char* buf, int64_t read_size);
    int64_t Write(const char* buf, int64_t write_size);
    bool Flush();
    bool Sync();
    bool Close();
    friend class FSImpl;
private:
    FSImpl* _fs;                        ///< �ļ�ϵͳ
    std::string _name;                  ///< �ļ�·��
    int32_t _open_flags;                ///< ��ʹ�õ�flag

    /// for write
    ChunkServer_Stub* _chains_head;     ///< ��Ӧ�ĵ�һ��chunkserver
    LocatedBlock* _block_for_write;     ///< ����д��block
    WriteBuffer* _write_buf;            ///< ����д����

    /// for read
    LocatedBlocks _located_blocks;      ///< block meta for read
    ChunkServer_Stub* _chunkserver;     ///< located chunkserver
    int64_t _read_offset;               ///< ��ȡ��ƫ��
    bool _closed;                       ///< �Ƿ�ر�
    Mutex   _mu;
};

class FSImpl : public FS {
public:
    friend class BfsFileImpl;
    FSImpl() : _rpc_client(NULL), _nameserver(NULL) {
    }
    ~FSImpl() {
        delete _nameserver;
        delete _rpc_client;
    }
    bool ConnectNameServer(const char* nameserver) {
        if (nameserver != NULL) {
            _nameserver_address = nameserver;
        } else {
            _nameserver_address = FLAGS_nameserver;
        }
        _rpc_client = new RpcClient();
        bool ret = _rpc_client->GetStub(_nameserver_address, &_nameserver);
        return ret;
    }
    bool GetFileSize(const char* path, int64_t* file_size) {
        return false;
    }
    bool CreateDirectory(const char* path) {
        CreateFileRequest request;
        CreateFileResponse response;
        request.set_file_name(path);
        request.set_type(0755|(1<<9));
        request.set_sequence_id(0);
        bool ret = _rpc_client->SendRequest(_nameserver, &NameServer_Stub::CreateFile,
            &request, &response, 5, 3);
        if (!ret || response.status() != 0) {
            return false;
        } else {
            return true;
        }
    }
    bool ListDirectory(const char* path, BfsFileInfo** filelist, int *num) {
        common::timer::AutoTimer at(1, "ListDirectory", path);
        *filelist = NULL;
        *num = 0;
        ListDirectoryRequest request;
        ListDirectoryResponse response;
        request.set_path(path);
        request.set_sequence_id(0);
        bool ret = _rpc_client->SendRequest(_nameserver, &NameServer_Stub::ListDirectory,
            &request, &response, 5, 3);
        if (!ret) {
            printf("List fail: %s\n", path);
            return false;
        }
        if (response.files_size() != 0) {
            *num = response.files_size();
            *filelist = new BfsFileInfo[*num];
            for (int i = 0; i < *num; i++) {
                BfsFileInfo& binfo =(*filelist)[i];
                const FileInfo& info = response.files(i);
                binfo.ctime = info.ctime();
                binfo.mode = info.type();
                binfo.size = info.size();
                snprintf(binfo.name, sizeof(binfo.name), "%s", info.name().c_str());
            }
        }
        return true;
    }
    bool DeleteDirectory(const char*, bool) {
        return false;
    }
    bool Access(const char* path, int32_t mode) {
        StatRequest request;
        StatResponse response;
        request.set_path(path);
        request.set_sequence_id(0);
        bool ret = _rpc_client->SendRequest(_nameserver, &NameServer_Stub::Stat,
            &request, &response, 5, 3);
        if (!ret) {
            printf("Stat fail: %s\n", path);
            return false;
        }
        return (response.status() == 0);
    }
    bool Stat(const char* path, BfsFileInfo* fileinfo) {
        StatRequest request;
        StatResponse response;
        request.set_path(path);
        request.set_sequence_id(0);
        bool ret = _rpc_client->SendRequest(_nameserver, &NameServer_Stub::Stat,
            &request, &response, 5, 3);
        if (!ret) {
            fprintf(stderr, "Stat rpc fail: %s\n", path);
            return false;
        }
        if (response.status() == 0) {
            const FileInfo& info = response.file_info();
            fileinfo->ctime = info.ctime();
            fileinfo->mode = info.type();
            fileinfo->size = info.size();
            snprintf(fileinfo->name, sizeof(fileinfo->name), "%s", info.name().c_str());
            return true;
        }
        return false;
    }
    bool OpenFile(const char* path, int32_t flags, File** file) {
        common::timer::AutoTimer at(1, "OpenFile", path);
        bool ret = false;
        *file = NULL;
        if (flags == O_WRONLY) {
            CreateFileRequest request;
            CreateFileResponse response;
            request.set_file_name(path);
            request.set_sequence_id(0);
            request.set_type(0644);
            ret = _rpc_client->SendRequest(_nameserver, &NameServer_Stub::CreateFile,
                &request, &response, 5, 3);
            if (!ret || response.status() != 0) {
                fprintf(stderr, "Open file for write fail: %s, status= %d\n",
                    path, response.status());
                ret = false;
            } else {
                //printf("Open file %s\n", path);
                *file = new BfsFileImpl(this, path, flags);
            }
        } else if (flags == O_RDONLY) {
            FileLocationRequest request;
            FileLocationResponse response;
            request.set_file_name(path);
            request.set_sequence_id(0);
            ret = _rpc_client->SendRequest(_nameserver, &NameServer_Stub::GetFileLocation,
                &request, &response, 5, 3);
            if (ret && response.status() == 0) {
                BfsFileImpl* f = new BfsFileImpl(this, path, flags);
                f->_located_blocks.CopyFrom(response.blocks());
                *file = f;
                //printf("OpenFile success: %s\n", path);
            } else {
                //printf("GetFileLocation return %d\n", response.blocks_size());
            }
        } else {
            printf("Open flags only O_RDONLY or O_WRONLY\n");
            ret = false;
        }
        return ret;
    }
    int64_t WriteFile(BfsFileImpl* file, const char* buf, int64_t len) {
        if (file->_chains_head == NULL) {
            AddBlockRequest request;
            AddBlockResponse response;
            request.set_sequence_id(0);
            request.set_file_name(file->_name);
            bool ret = _rpc_client->SendRequest(_nameserver, &NameServer_Stub::AddBlock,
                &request, &response, 5, 3);
            if (!ret || !response.has_block()) {
                printf("AddBlock fail for %s\n", file->_name.c_str());
                return -1;
            }
            file->_block_for_write = new LocatedBlock(response.block());
            const std::string& addr = file->_block_for_write->chains(0).address();
            //printf("response addr %s\n", response.block().chains(0).address().c_str());
            //printf("_block_for_write addr %s\n", 
            //        file->_block_for_write->chains(0).address().c_str());
            _rpc_client->GetStub(addr, &file->_chains_head);
            file->_write_buf = new WriteBuffer(256*1024);
        }
        int w = 0;
        while (w < len) {
            if ( (len - w) < file->_write_buf->Available()) {
                file->_write_buf->Append(buf+w, len-w);
                w = len;
                break;
            } else {
                int n = file->_write_buf->Available();
                file->_write_buf->Append(buf+w, n);
                w += n;
            }
            if (file->_write_buf->Available() == 0) {
                if (!WriteChunk(file->_chains_head, file->_write_buf,
                                file->_block_for_write, false)) {
                    w = -1;
                    break;
                }
                file->_write_buf->Clear();
            }
        }
        // printf("Write return %d, buf_size=%d\n", w, file->_write_buf->Size());
        return w;
    }
    /// Send local buffer to chunkserver
    bool WriteChunk(ChunkServer_Stub* stub, WriteBuffer* write_buf,
                    LocatedBlock* block, bool is_last) {
        WriteBlockRequest request;
        WriteBlockResponse response;
        int64_t offset = block->block_size();
        int64_t seq = common::timer::get_micros();
        request.set_sequence_id(seq);
        request.set_block_id(block->block_id());
        request.set_offset(offset);
        request.set_is_last(is_last);
        for (int i = 1; i < block->chains_size(); i++) {
            request.add_chunkservers(block->chains(i).address());
        }
        request.set_databuf(write_buf->Data(), write_buf->Size());
        request.set_offset(block->block_size());
        //printf("WriteBlock %ld [%ld:%ld:%d]\n", seq, block->block_id(),
        //       block->block_size(), write_buf->Size());
        if (!_rpc_client->SendRequest(stub, &ChunkServer_Stub::WriteBlock,
            &request, &response, 60, 1) || response.status() != 0) {
            printf("WriteBlock fail %ld [%ld:%ld:%d]: %s, status=%d\n",
                   seq, block->block_id(), block->block_size(), write_buf->Size(),
                   response.has_bad_chunkserver()?response.bad_chunkserver().c_str():"unknown",
                   response.status());
            assert(0);
            return false;
        }
        block->set_block_size(offset + write_buf->Size());
        return true;
    }
    bool CloseFile(File* file) {
        return file->Close();
    }
    bool DeleteFile(const char* path) {
        UnlinkRequest request;
        UnlinkResponse response;
        request.set_path(path);
        int64_t seq = common::timer::get_micros();
        request.set_sequence_id(seq);
        // printf("Delete file: %s\n", path);
        bool ret = _rpc_client->SendRequest(_nameserver, &NameServer_Stub::Unlink,
            &request, &response, 5, 1);
        if (!ret) {
            fprintf(stderr, "Unlink rpc fail: %s to %s\n", path);
            return false;
        }
        if (response.status() != 0) {
            fprintf(stderr, "Unlink %s return: %d\n", path, response.status());
            return false;
        }
        return true;
    }
    bool Rename(const char* oldpath, const char* newpath) {
        RenameRequest request;
        RenameResponse response;
        request.set_oldpath(oldpath);
        request.set_newpath(newpath);
        request.set_sequence_id(0);
        bool ret = _rpc_client->SendRequest(_nameserver, &NameServer_Stub::Rename,
            &request, &response, 5, 3);
        if (!ret) {
            fprintf(stderr, "Rename rpc fail: %s to %s\n", oldpath, newpath);
            return false;
        }
        if (response.status() != 0) {
            fprintf(stderr, "Rename %s to %s return: %d\n",
                oldpath, newpath, response.status());
            return false;
        }
        return true;
    }
private:
    RpcClient* _rpc_client;
    NameServer_Stub* _nameserver;
    std::string _nameserver_address;
};

BfsFileImpl::BfsFileImpl(FSImpl* fs, const std::string name, int32_t flags)
  : _fs(fs), _name(name), _open_flags(flags), _chains_head(NULL),
    _chunkserver(NULL), _read_offset(0), _closed(false) {
}

BfsFileImpl::~BfsFileImpl () {
    if (!_closed) {
        _fs->CloseFile(this);
    }
}

int64_t BfsFileImpl::Pread(char* buf, int64_t read_len, int64_t offset) {
    MutexLock lock(&_mu);
     
    if (_located_blocks._blocks.empty() || _located_blocks._blocks[0].chains_size() == 0) {
        printf("No located servers or _located_blocks[%lu]\n",_located_blocks._blocks.size());
        return -3;
    }
    LocatedBlock& lcblock = _located_blocks._blocks[0];
    int64_t block_id = lcblock.block_id();
    if (_chunkserver == NULL) {
        _fs->_rpc_client->GetStub(lcblock.chains(0).address(), &_chunkserver);
    }
    ReadBlockRequest request;
    ReadBlockResponse response;
    request.set_sequence_id(0);
    request.set_block_id(block_id);
    request.set_offset(offset);
    request.set_read_len(read_len);
    bool ret = _fs->_rpc_client->SendRequest(_chunkserver, &ChunkServer_Stub::ReadBlock,
    &request, &response, 5, 3);
    if (!ret || response.status() != 0) {
        printf("Read block %ld fail, status= %d\n", block_id, response.status());
        return -4;
    }

    //printf("Pread[%s:%ld:%ld] return %lu bytes\n",
    //       _name.c_str(), offset, read_len, response.databuf().size());
    int64_t ret_len = response.databuf().size();
    assert(read_len >= ret_len);
    memcpy(buf, response.databuf().data(), ret_len);
    return ret_len;
}

int64_t BfsFileImpl::Seek(int64_t offset, int32_t whence) {
    //printf("Seek[%s:%d:%ld]\n", _name.c_str(), whence, offset);
    if (_open_flags != O_RDONLY) {
        return -2;
    }
    if (whence == SEEK_SET) {
        _read_offset = offset;
    } else if (whence == SEEK_CUR) {
        _read_offset += offset;
    } else {
        return -1;
    }
    return _read_offset;
}

int64_t BfsFileImpl::Read(char* buf, int64_t read_len) {
    //printf("[%p] Read[%s:%ld] offset= %ld\n", this, _name.c_str(), read_len, _read_offset);
    if (_open_flags != O_RDONLY) {
        return -2;
    }
    int ret = Pread(buf, read_len, _read_offset);
    if (ret >= 0) {
        _read_offset += ret;
    }
    //printf("[%p] Read[%s:%ld] return %d offset=%ld\n", this, _name.c_str(), read_len, ret, _read_offset);
    return ret;
}

int64_t BfsFileImpl::Write(const char* buf, int64_t write_size) {
    common::timer::AutoTimer at(100, "Write", _name.c_str());
    if (_open_flags != O_WRONLY) {
        return -2;
    }
    MutexLock lock(&_mu);
    return _fs->WriteFile(this, buf, write_size);
}
bool BfsFileImpl::Flush() {
    // Not impliment
    return Sync();
}
bool BfsFileImpl::Sync() {
    common::timer::AutoTimer at(100, "Sync", _name.c_str());
    if (_open_flags != O_WRONLY) {
        return false;
    }
    MutexLock lock(&_mu);
    if (_write_buf->Size() == 0) {
        return true;
    }
    bool ret = _fs->WriteChunk(_chains_head, _write_buf, _block_for_write, false);
    if (ret) {
        _write_buf->Clear();
    }
    // fprintf(stderr, "Sync %s fail\n", _name.c_str());
    return ret;
}

bool BfsFileImpl::Close() {
    bool ret = true;
    if (_open_flags == O_WRONLY) {
        ret =  _fs->WriteChunk(_chains_head, _write_buf, _block_for_write, true);
    }
    _closed = true;
    return true;
}

bool FS::OpenFileSystem(const char* nameserver, FS** fs) {
    FSImpl* impl = new FSImpl;
    if (!impl->ConnectNameServer(nameserver)) {
        *fs = NULL;
        return false;
    }
    *fs = impl;
    return true;
}

}

/* vim: set expandtab ts=4 sw=4 sts=4 tw=100: */