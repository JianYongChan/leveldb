#include <iostream>
#include <cassert>
#include <string>
#include <utility>
#include <leveldb/db.h>

static void test_1() {
    leveldb::DB *db = NULL;
    leveldb::Options options;
    options.create_if_missing = true;

    leveldb::Status status = leveldb::DB::Open(options,
     "/home/ctom/ilinux/Code/SourceCodeReading/LevelDB/TestDB/DB/DB1", &db);
     assert(status.ok());

     std::string key   = "lover";
     std::string value = "xibei";
     std::string get_value;

     leveldb::Status s = db->Put(leveldb::WriteOptions(), key, value);
     if (s.ok()) {
         s = db->Get(leveldb::ReadOptions(), "lover", &get_value);
     }
     if (s.ok()) {
         std::cout << get_value << std::endl;
     } else {
         std::cout << s.ToString() << std::endl;
     }
     s = db->Delete(leveldb::WriteOptions(), "lover");
     if (s.ok()) {
         std::cout << "delete lover" << std::endl;
     } else {
         std::cout << s.ToString() << std::endl;
     }
}

int main() {
    test_1();

    return 0;
}