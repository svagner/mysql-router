/*
  Copyright (c) 2016, Oracle and/or its affiliates. All rights reserved.

  This program is free software; you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation; version 2 of the License.

  This program is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
  GNU General Public License for more details.

  You should have received a copy of the GNU General Public License
  along with this program; if not, write to the Free Software
  Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA
*/

#include "keyring/keyring_manager.h"
#include "keyring/keyring_file.h"
#include "filesystem.h"
#include "common.h"
#include "dim.h"
#include "my_aes.h"
#include "utils.h"  // FIXME: cross-depenency (we're including Router stuff from Harness)
#include <string.h>
#include <random>
#include <system_error>
#include <fstream>
#ifndef _WIN32
#include <sys/stat.h>
#endif


/*
 * Keyring Management
 *
 * One or more passwords can be stored in the keyring, which is persisted on
 * disk in the keyring file.
 * The encryption key of the keyring can be fed to the keyring through an
 * auto-generated and persisted on a master key file
 *
 * The keyring's encryption key will be itself
 * encrypted by a second key, which is generated automatically and stored
 * in the keyring file. The location of the master key file is selected by the
 * user and the same key file can be shared by multiple keyrings.
 *
 * File Layout:
 *
 *  Keyring File                 KeyFile
 * +-------------+             +-------------------+
 * | KeyFile Key |             | Keyring File Name |
 * |-------------|             | Keyring Key       |
 * | Password    |             | Keyring File Name |
 * | Password    |             | Keyring Key       |
 * | ...         |             +-------------------+
 * +-------------+
 */

namespace mysql_harness {

static std::unique_ptr<KeyringFile> g_keyring;
static std::string g_keyring_file_path;
static std::string g_keyring_key;

constexpr auto kAesMode = myaes::my_aes_256_cbc;
constexpr unsigned char kAesIv[] = {
    0x39, 0x62, 0x9f, 0x52, 0x7f, 0x76, 0x9a, 0xae,
    0xcd, 0xca, 0xf7, 0x04, 0x65, 0x8e, 0x5d, 0x88
};

static const unsigned kKeyLength = 32;
static const char kMasterKeyFileSignature[] = "MRKF";

class MasterKeyFile {
public:
  MasterKeyFile(const std::string &file)
      : path_(file) {
  }

  void load() {
    std::ifstream f;
    if (Path(path_).is_directory())
      throw invalid_master_keyfile(path_+" is a directory");
    f.open(path_, std::ios_base::binary|std::ios_base::in);
    if (f.fail()) {
      throw std::system_error(std::error_code(errno, std::system_category()), "Can't open file "+path_);
    }
    char buf[sizeof(kMasterKeyFileSignature)];
    f.read(buf, sizeof(buf));
    if (strncmp(buf, kMasterKeyFileSignature, sizeof(kMasterKeyFileSignature)) != 0)
      throw invalid_master_keyfile("Invalid master key file "+path_);
    entries_.clear();
    try {
      while (!f.eof()) {
        uint32_t length;
        f.read(reinterpret_cast<char*>(&length), sizeof(length));
        std::string data;
        data.resize(length);
        f.read(&data[0], static_cast<std::streamsize>(data.size()));
        std::string n, v;
        n = std::string(data.data(), strlen(data.data()));
        v = data.substr(n.size()+1);
        entries_.push_back(std::make_pair(n, v));
      }
    } catch (std::exception &e) {
      throw std::runtime_error("Error reading from master key file "+path_+
          ": "+e.what());
    }
    f.close();
  }

  void save() {
    std::ofstream f;
    f.open(path_, std::ios_base::binary|std::ios_base::trunc|std::ios_base::out);
    if (f.fail()) {
      throw std::runtime_error("Could not open master key file "+path_+
          ": "+get_strerror(errno));
    }
    try {
      make_file_private(path_);
    }
    catch (std::exception &e) {
      throw std::runtime_error("Could not set permissions of master key file " + path_ +
        ": " + e.what());
    }
    f.write(kMasterKeyFileSignature, sizeof(kMasterKeyFileSignature));
    for (auto &entry : entries_) {
      uint32_t length = static_cast<uint32_t>(entry.first.length() + entry.second.length() + 1);
      f.write(reinterpret_cast<char*>(&length), sizeof(length));
      // write name of the entry
      f.write(entry.first.data(), static_cast<std::streamsize>(entry.first.length()+1));
      // write encrypted entry data
      f.write(entry.second.data(), static_cast<std::streamsize>(entry.second.length()));
    }
    f.close();
  }

  void add(const std::string &id, const std::string &value,
           const std::string &key) {
    auto aes_buffer_size = myaes::my_aes_get_size(
        static_cast<uint32_t>(value.length()), myaes::my_aes_256_cbc);
    std::vector<char> aes_buffer(static_cast<size_t>(aes_buffer_size));

    auto encrypted_size = myaes::my_aes_encrypt(
        reinterpret_cast<const unsigned char*>(value.data()),
        static_cast<uint32_t>(value.length()),
        reinterpret_cast<unsigned char*>(aes_buffer.data()),
        reinterpret_cast<const unsigned char*>(key.data()),
        static_cast<uint32_t>(key.length()), myaes::my_aes_256_cbc, kAesIv);
    if (encrypted_size < 0) {
      throw std::runtime_error("Could not encrypt master key data");
    }
    aes_buffer.resize(static_cast<std::size_t>(encrypted_size));
    entries_.push_back(std::make_pair(id, std::string(&aes_buffer[0], aes_buffer.size())));
  }

  std::string get(const std::string &id, const std::string &key) {
    for (auto &entry : entries_) {
      if (entry.first == id) {
        std::vector<char> decrypted_buffer(entry.second.size());

        auto decrypted_size = my_aes_decrypt(
            reinterpret_cast<const unsigned char*>(entry.second.data()),
            static_cast<uint32_t>(entry.second.length()),
            reinterpret_cast<unsigned char*>(decrypted_buffer.data()),
            reinterpret_cast<const unsigned char*>(key.data()),
            static_cast<uint32_t>(key.length()), kAesMode, kAesIv);

        if (decrypted_size < 0)
          throw decryption_error("Decryption failed.");

        return std::string(&decrypted_buffer[0], decrypted_buffer.size());
      }
    }
    return "";
  }

private:
  std::string path_;
  std::vector<std::pair<std::string, std::string>> entries_;
};


/**
 * Gets the master_key for the specified keyring_file from the master key store.
 * If the master key store file does not exist, it will be created along with
 * a new master_key, which will be stored and also returned.
 * If the master key store already exists, but does not have an entry for the
 * master key, it will be generated and then stored.
 *
 * Returns the master_key and the scramble for the master_key
 */
static std::pair<std::string,std::string>
    get_master_key(MasterKeyFile &mkf,
                   const std::string &keyring_file_path,
                   bool create_if_needed) {
  KeyringFile kf;

  // get the scramble for the master key file from the keyring file itself
  std::string master_scramble;

  try {
    master_scramble = kf.read_header(keyring_file_path);
    if (master_scramble.empty()) {
      throw std::runtime_error("Keyring file '"+keyring_file_path+"' was created in an old version and needs to be recreated. Please delete and bootstrap again.");
    }
  } catch (std::exception&) {
    if (errno != ENOENT || !create_if_needed)
      throw;
  }
  std::string master_key;
  // get the key for the keyring from the master key file, decrypting it with
  // the scramble
  if (!master_scramble.empty()) {
    try {
      // look up for the master_key for this given keyring file
      master_key = mkf.get(keyring_file_path, master_scramble);
    } catch (std::out_of_range&) {
      // missing key will be handled further down
    }
  }
  if (master_key.empty()) {
    if (!create_if_needed)
      throw std::runtime_error("Master key for keyring at '" + keyring_file_path + "' could not be read");
    // if the master key doesn't exist anywhere yet, generate one and store it
    mysqlrouter::RandomGeneratorInterface& rg = mysql_harness::DIM::instance().get_RandomGenerator();
    master_key = rg.generate_password(kKeyLength);
    // scramble to encrypt the master key with, which should be stored in the
    // keyring
    master_scramble = rg.generate_password(kKeyLength);
    mkf.add(keyring_file_path, master_key, master_scramble);
  }
  return std::make_pair(master_key, master_scramble);
}

bool init_keyring(const std::string &keyring_file_path,
                  const std::string &master_key_path,
                  bool create_if_needed) {
  std::string master_key;
  std::string master_scramble;
  MasterKeyFile mkf(master_key_path);

  try {
    mkf.load();
  } catch (std::exception&) {
    if (errno == ENOENT && create_if_needed) {
      // ignore the error and proceed to create the file
    } else
      throw;
  }

  std::tie(master_key, master_scramble) = get_master_key(mkf,
                                                         keyring_file_path,
                                                         create_if_needed);
  bool existed = init_keyring_with_key(keyring_file_path, master_key, create_if_needed);
  if (create_if_needed && !existed) {
    g_keyring->set_header(master_scramble);
    flush_keyring();
    try {
      mkf.save();
    } catch (...) {
      throw std::runtime_error("Unable to save master key to " + master_key_path
          + ": " + get_strerror(errno));
    }
  }
  return existed;
}

bool init_keyring_with_key(const std::string &keyring_file_path,
                           const std::string &master_key,
                           bool create_if_needed) {
  if (g_keyring)
    throw std::logic_error("Keyring already initialized");
  bool existed = false;
  std::unique_ptr<KeyringFile> key_store(new KeyringFile());
  try {
    key_store->load(keyring_file_path, master_key);
    existed = true;
  } catch (std::exception &e) {
    if (!create_if_needed)
      throw;
    // force initial creation
    key_store->save(keyring_file_path, master_key);
  }
  g_keyring = std::move(key_store);
  g_keyring_file_path = keyring_file_path;
  g_keyring_key = master_key;
  return existed;
}

void flush_keyring() {
  if (!g_keyring)
    throw std::logic_error("No keyring loaded");
  g_keyring->save(g_keyring_file_path, g_keyring_key);
}

Keyring *get_keyring() {
  return g_keyring.get();
}

void reset_keyring() {
  g_keyring.reset();
}


}
