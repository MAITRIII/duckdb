#define DUCKDB_EXTENSION_MAIN

#include "alex_extension.hpp"
#include "duckdb.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/function/scalar_function.hpp"
#include "duckdb/main/extension_util.hpp"
#include <duckdb/parser/parsed_data/create_scalar_function_info.hpp>

#include "duckdb/catalog/catalog_entry/table_catalog_entry.hpp"
#include "duckdb/catalog/catalog_search_path.hpp"
#include "duckdb/main/database.hpp"
#include "duckdb/catalog/catalog.hpp"
#include "duckdb/main/client_data.hpp"
#include "duckdb/main/query_result.hpp"
#include "duckdb/common/types/vector.hpp"
#include "duckdb/main/client_data.hpp"
#include "duckdb/main/materialized_query_result.hpp"

#include <duckdb/parser/parsed_data/create_scalar_function_info.hpp>
#include "duckdb/catalog/catalog_search_path.hpp"
#include "duckdb/main/database.hpp"
#include "duckdb/catalog/catalog.hpp"
#include "duckdb/common/types/vector.hpp"

//Radix Spline
#include "builder.h"
#include "radix_spline.h"
#include "serializer.h"
#include "common.h"

#include <fstream>
#include <iostream>
#include <vector>
#include <algorithm>

// OpenSSL linked through vcpkg
#include <openssl/opensslv.h>
#include<map>
#include "ALEX/src/core/alex.h"
#include "utils.h"
#include <chrono>
#include <numeric>
#include<iomanip>
#include <iostream>
#include "pgm/pgm_index.hpp"
#include "pgm/pgm_index_dynamic.hpp"
#define DOUBLE_KEY_TYPE double
#define GENERAL_PAYLOAD_TYPE double
#define KEY_TYPE int

#define INDEX_PAYLOAD_TYPE double

#define INT64_KEY_TYPE int64_t
#define INT_KEY_TYPE int
#define UNSIGNED_INT64_KEY_TYPE uint64_t
#define PAYLOAD_TYPE double

#define HUNDRED_MILLION 100000000
#define TEN_MILLION 10000000


namespace duckdb {

// ALEX Index instances
alex::Alex<DOUBLE_KEY_TYPE, INDEX_PAYLOAD_TYPE> double_alex_index;
alex::Alex<INT64_KEY_TYPE, INDEX_PAYLOAD_TYPE> big_int_alex_index;
alex::Alex<UNSIGNED_INT64_KEY_TYPE, INDEX_PAYLOAD_TYPE> unsigned_big_int_alex_index;
alex::Alex<INT_KEY_TYPE, INDEX_PAYLOAD_TYPE> int_alex_index;

// PGM Index instances
pgm::DynamicPGMIndex<double, double> double_dynamic_index;
pgm::DynamicPGMIndex<INT64_KEY_TYPE, INDEX_PAYLOAD_TYPE> big_int_dynamic_index;
pgm::DynamicPGMIndex<UNSIGNED_INT64_KEY_TYPE, INDEX_PAYLOAD_TYPE> unsigned_big_int_dynamic_index;
pgm::DynamicPGMIndex<INT_KEY_TYPE, INDEX_PAYLOAD_TYPE> int_dynamic_index;

// Global variables
std::map<std::string, std::pair<std::string, std::string>> index_type_table_name_map;
std::vector<std::vector<unique_ptr<Base>>> results;
int load_end_point = 0;

/*
* Struture to store the stats of RadixSpline efficiently
*/
struct RadixSplineStats {
    size_t num_keys;
    uint64_t min_key;
    uint64_t max_key;
    double average_gap;

    RadixSplineStats() : num_keys(0), min_key(0), max_key(0), average_gap(0.0) {}
};

// RadixSpline related variables
const size_t kNumRadixBits = 18;
const size_t kMaxError = 32;

// Separate maps for different key types, for each table
std::map<std::string, rs::RadixSpline<uint32_t>> radix_spline_map_int32;
std::map<std::string, rs::RadixSpline<uint64_t>> radix_spline_map_int64;

// Maps for storing the stats efficiently
std::map<std::string, RadixSplineStats> radix_spline_stats_map_int32;
std::map<std::string, RadixSplineStats> radix_spline_stats_map_int64;

// Helper functions
static QualifiedName GetQualifiedName(ClientContext &context, const std::string &qname_str) {
    auto qname = QualifiedName::Parse(qname_str);
    if (qname.schema == INVALID_SCHEMA) {
        qname.schema = ClientData::Get(context).catalog_search_path->GetDefaultSchema(qname.catalog);
    }
    return qname;
}

// Dummy function for testing
inline void AlexDummy(DataChunk &args, ExpressionState &state, Vector &result) {
    std::cout << "Dummy function called\n";
}

// Scalar function to prepend "Alex" to input strings
inline void AlexScalarFun(DataChunk &args, ExpressionState &state, Vector &result) {
    auto &name_vector = args.data[0];
    UnaryExecutor::Execute<string_t, string_t>(
        name_vector, result, args.size(),
        [&](string_t name) {
            return StringVector::AddString(result, "Alex " + name.GetString() + " 🐥");
        });
}

// Scalar function to return linked OpenSSL version
inline void AlexOpenSSLVersionScalarFun(DataChunk &args, ExpressionState &state, Vector &result) {
    auto &name_vector = args.data[0];
    UnaryExecutor::Execute<string_t, string_t>(
        name_vector, result, args.size(),
        [&](string_t name) {
            return StringVector::AddString(
                result, "Alex " + name.GetString() + ", my linked OpenSSL version is " + OPENSSL_VERSION_TEXT);
        });
}

void functionTryAlex(){}

template<typename T> 
void printElement(T t, const int& width){
    const char separator    = ' ';
    std::cout << std::setw(width) << std::setfill(separator) << t;
}

void display_row(int row_id,vector<string>columnNames){
    const std::vector<unique_ptr<Base>>& vec = results.at(row_id);
    int num_width = 10;

    for(string colName:columnNames){
        printElement(colName,num_width);
    }
    std::cout<<"\n";
    for(int i=0;i<vec.size();i++){
        if (auto* intData = dynamic_cast<IntData*>(vec[i].get())) {
            printElement(intData->value,num_width);
        } else if (auto* doubleData = dynamic_cast<DoubleData*>(vec[i].get())) {
            printElement(doubleData->value,num_width);
        } else if (auto* stringData = dynamic_cast<StringData*>(vec[i].get())) {
            printElement(stringData->value,num_width);
        } else if (auto* boolData = dynamic_cast<BoolData*>(vec[i].get())) {
            printElement(boolData->value,num_width);
        }
    }
}

void executeQuery(duckdb::Connection& con,string QUERY){
    unique_ptr<MaterializedQueryResult> result = con.Query(QUERY);
    if(result->HasError()){
        std::cout<<"Query execution failed "<<"\n";
    }
    else{
        std::cout<<"Query execution successful! "<<"\n";
    }
}

/**
 * Loading Benchmark into the tables of DuckDB.
 * 
*/

template <typename K,typename P>
void load_benchmark_data_into_table(std::string benchmarkFile,std::string benchmarkFileType,duckdb::Connection& con,std::string tableName,int NUM_KEYS,int num_batches_insert,int per_batch){
    //This function will load a key and payload type agnostic data into the database.
    int starting = 0;
    int ending = 0;

    auto keys = new K[NUM_KEYS];
    bool res = load_binary_data(keys,NUM_KEYS,benchmarkFile);

    std::cout<<"Res of loading from benchmark file "<<res<<"\n"; 

    string query = "INSERT INTO "+tableName+" VALUES ";


    for(int i=0;i<num_batches_insert;i++){
        std::cout<<"Inserting batch "<<i<<"\n";
        
        //KeyType batch_keys[per_batch];  // Replace KeyType with the actual type of keys
        starting = i*per_batch;

        ending = starting + per_batch;
        string tuple_string = "";

        // std::cout<<"Starting "<<starting<<" Ending "<<ending<<"\n";
        
        auto values = new std::pair<K, P>[per_batch];
        std::mt19937_64 gen_payload(std::random_device{}());


        for (int vti = starting; vti < ending; vti++) {
            //values[vti].first = keys[vti];
            K key = keys[vti];
            P random_payload = static_cast<P>(gen_payload());
            //std::cout<<"dae key "<<key<<"\n";
            std::ostringstream stream;
            if(typeid(K)==typeid(DOUBLE_KEY_TYPE)){
                stream << std::setprecision(std::numeric_limits<K>::max_digits10) << key;
            }
            else{
                stream << key;
            }
            std::string ressy = stream.str();
            tuple_string = tuple_string + "(" + ressy + "," + std::to_string(random_payload) + ")";
            //std::cout<<"Tuple string "<<tuple_string<<"\n";
            if(vti!=ending-1){
                tuple_string = tuple_string + ",";
            }
        }
        string to_execute_query = query + tuple_string + ";";

        auto res = con.Query(to_execute_query);
        if(!res->HasError()){
            std::cout<<"Batch inserted successfully "<<"\n";
        }else{
            std::cout<<"Error inserting batch "<<i<<"\n";
        }
    }
}

void functionLoadBenchmark(ClientContext &context, const FunctionParameters &parameters){
    std::string tableName = parameters.values[0].GetValue<string>();
    std::string benchmarkName = parameters.values[1].GetValue<string>();
    int benchmark_size = parameters.values[2].GetValue<int>();
    int num_batches_insert = parameters.values[3].GetValue<int>();

    std::cout<<"Loading benchmark data - "<<benchmarkName<<"into table "<<tableName<<"\n";
    std::cout<<"The schema of the table will be {key,payload}\n";
    std::cout<<"Number of keys  "<<benchmark_size<<"\n";
    
    load_end_point = benchmark_size;
    std::string benchmarkFile = "";
    std::string benchmarkFileType = "";
    const int NUM_KEYS = benchmark_size;

    //Establish a connection with the Database.
    duckdb::Connection con(*context.db);

    /**
     * Create a table with the table name.
    */
    std::string CREATE_QUERY = "";
    
    //int num_batches_insert = 1000;
    int per_batch = NUM_KEYS/num_batches_insert;
    std::cout<<"Per batch insertion "<<per_batch<<"\n";
    
    std::cout<<"Benchmark name "<<benchmarkName<<"\n";
    if(benchmarkName.compare("lognormal")==0){
        benchmarkFile = "/Users/jishnusm/Desktop/classes/AdvancedDataStores/Project/Project2/radix/test/lognormal-190M.bin";
        benchmarkFileType = "binary";
        CREATE_QUERY = "CREATE TABLE "+tableName+"(key BIGINT, payload double);";
        executeQuery(con,CREATE_QUERY);
        load_benchmark_data_into_table<INT64_KEY_TYPE,GENERAL_PAYLOAD_TYPE>(benchmarkFile,benchmarkFileType,con,tableName,NUM_KEYS,num_batches_insert,per_batch);
    }
    else if(benchmarkName.compare("longitudes")==0){
        benchmarkFile = "/Users/jishnusm/Desktop/classes/AdvancedDataStores/Project/Project2/radix/test/longitudes-200M.bin";
        benchmarkFileType = "binary";
        CREATE_QUERY = "CREATE TABLE "+tableName+"(key double, payload double);";
        executeQuery(con,CREATE_QUERY);
        load_benchmark_data_into_table<DOUBLE_KEY_TYPE,GENERAL_PAYLOAD_TYPE>(benchmarkFile,benchmarkFileType,con,tableName,NUM_KEYS,num_batches_insert,per_batch);
    }
    else if(benchmarkName.compare("longlat")==0){
        benchmarkFile = "/Users/jishnusm/Desktop/classes/AdvancedDataStores/Project/Project2/radix/test/longlat-200M.bin";
        benchmarkFileType = "binary";
        CREATE_QUERY = "CREATE TABLE "+tableName+"(key double, payload double);";
        executeQuery(con,CREATE_QUERY);
        load_benchmark_data_into_table<DOUBLE_KEY_TYPE,GENERAL_PAYLOAD_TYPE>(benchmarkFile,benchmarkFileType,con,tableName,NUM_KEYS,num_batches_insert,per_batch);
    }
    else if(benchmarkName.compare("ycsb")==0){
        benchmarkFile = "/Users/jishnusm/Desktop/classes/AdvancedDataStores/Project/Project2/radix/test/ycsb-200M.bin.data";
        benchmarkFileType = "binary";
        std::cout<<"Table name "<<tableName<<"\n";
        CREATE_QUERY = "CREATE TABLE "+tableName+"(key UBIGINT , payload double);";
        executeQuery(con,CREATE_QUERY);
        //Args: Benchmark Key Type, Benchmark Payload Type, Benchmark File, Benchmark File Type, conn object, table name, NUM_KEYS,num_batches_insert, per_batch
        load_benchmark_data_into_table<UNSIGNED_INT64_KEY_TYPE,GENERAL_PAYLOAD_TYPE>(benchmarkFile,benchmarkFileType,con,tableName,NUM_KEYS,num_batches_insert,per_batch);
    }

}

double calculateAverage(const std::vector<double>& v) {
    return std::accumulate(v.begin(), v.end(), 0.0) / v.size();
}

/**
 * 
 * Function to run one batch of benchmark - Lookup
*/

/**
 * 
 * Correctness verification :)
*/
template <typename K>
void runLookupBenchmarkOneBatchAlex(duckdb::Connection &con,std::string table_name){
    std::cout<<"Hey this is the general template";
}
template<>
void runLookupBenchmarkOneBatchAlex<INT64_KEY_TYPE>(duckdb::Connection& con,std::string table_name){
    std::cout<<"Running benchmark with one batch";
    /*
    My rationale here - I will run the benchmark for one batch - read a defined number of keys and count the time needed to do that.
    */

   // Create a random number generator
    std::random_device rd;
    std::mt19937 g(rd());

   vector<double> payloads;
   vector<INT64_KEY_TYPE>keys;
   for(int i=0;i<results.size();i++){
        std::vector<duckdb::unique_ptr<duckdb::Base>>& vec = results.at(i);
        keys.push_back(dynamic_cast<BigIntData*>(vec[0].get())->value);
        payloads.push_back(dynamic_cast<DoubleData*>(vec[1].get())->value);
    }
    double sum = 0;
    // for(int i=0;i<payloads.size();i++){
    //     sum += payloads[i];
    // }
    // std::cout<<"Sum of payloads "<<sum<<"\n";
    // std::cout<<"Average "<<sum/payloads.size()<<"\n";

    std::shuffle(keys.begin(), keys.end(), g);
    std::cout<<"Keys have been shuffled!\n";
    auto start = std::chrono::high_resolution_clock::now();
    for(int i=0;i<keys.size();i++){
        auto key = keys[i];
        auto it = big_int_alex_index.find(key);
        if (it != big_int_alex_index.end()) {
            double payload = it.payload();
            sum+=payload;
        }
    }
    std::cout<<"Average : "<<sum/keys.size()<<"\n";
    auto end = std::chrono::high_resolution_clock::now();
    std::chrono::duration<double> elapsed_seconds = end - start;
    std::cout << "Time taken to lookup "<<results.size()<<" keys is "<< elapsed_seconds.count() << " seconds\n";
    std::cout<<"Checking Correctness: \n";
    std::string query = "SELECT AVG(payload) FROM "+table_name+";";

    start = std::chrono::high_resolution_clock::now();
    auto res = con.Query(query);
    if(!res->HasError()){
        res->Print();
    }
    end = std::chrono::high_resolution_clock::now();
    elapsed_seconds = end - start;
    std::cout << "Time taken to avg from DuckDB is "<<results.size()<<" keys is "<< elapsed_seconds.count() << " seconds\n";
}

template<>
void runLookupBenchmarkOneBatchAlex<DOUBLE_KEY_TYPE>(duckdb::Connection& con,std::string table_name){
    std::cout<<"Running benchmark with one batch";
    /*
    My rationale here - I will run the benchmark for one batch - read a defined number of keys and count the time needed to do that.
    */

   // Create a random number generator
    std::random_device rd;
    std::mt19937 g(rd());

   vector<double> payloads;
   vector<DOUBLE_KEY_TYPE>keys;
   for(int i=0;i<results.size();i++){
        std::vector<duckdb::unique_ptr<duckdb::Base>>& vec = results.at(i);
        keys.push_back(dynamic_cast<DoubleData*>(vec[0].get())->value);
        payloads.push_back(dynamic_cast<DoubleData*>(vec[1].get())->value);
    }
    double sum = 0;
    // for(int i=0;i<payloads.size();i++){
    //     sum += payloads[i];
    // }
    // std::cout<<"Sum of payloads "<<sum<<"\n";
    // std::cout<<"Average "<<sum/payloads.size()<<"\n";

    std::shuffle(keys.begin(), keys.end(), g);
    std::cout<<"Keys have been shuffled!\n";
    auto start = std::chrono::high_resolution_clock::now();
    for(int i=0;i<keys.size();i++){
        auto key = keys[i];
        auto it = double_alex_index.find(key);
        if (it != double_alex_index.end()) {
            double payload = it.payload();
            sum+=payload;
        }
    }
    std::cout<<"Average : "<<sum/keys.size()<<"\n";
    auto end = std::chrono::high_resolution_clock::now();
    std::chrono::duration<double> elapsed_seconds = end - start;
    std::cout << "Time taken to lookup "<<results.size()<<" keys is "<< elapsed_seconds.count() << " seconds\n";
    std::cout<<"Checking Correctness: \n";
    std::string query = "SELECT AVG(payload) FROM "+table_name+";";

    start = std::chrono::high_resolution_clock::now();
    auto res = con.Query(query);
    if(!res->HasError()){
        res->Print();
    }
    end = std::chrono::high_resolution_clock::now();
    elapsed_seconds = end - start;
    std::cout << "Time taken to avg from DuckDB is "<<results.size()<<" keys is "<< elapsed_seconds.count() << " seconds\n";
}

template<>
void runLookupBenchmarkOneBatchAlex<UNSIGNED_INT64_KEY_TYPE>(duckdb::Connection& con,std::string table_name){
    std::cout<<"Running benchmark with one batch";
    /*
    My rationale here - I will run the benchmark for one batch - read a defined number of keys and count the time needed to do that.
    */

   // Create a random number generator
    std::random_device rd;
    std::mt19937 g(rd());

   vector<double> payloads;
   vector<UNSIGNED_INT64_KEY_TYPE>keys;
   for(int i=0;i<results.size();i++){
        std::vector<duckdb::unique_ptr<duckdb::Base>>& vec = results.at(i);
        keys.push_back(dynamic_cast<UBigIntData*>(vec[0].get())->value);
        payloads.push_back(dynamic_cast<DoubleData*>(vec[1].get())->value);
    }
    double sum = 0;
    // for(int i=0;i<payloads.size();i++){
    //     sum += payloads[i];
    // }
    // std::cout<<"Sum of payloads "<<sum<<"\n";
    // std::cout<<"Average "<<sum/payloads.size()<<"\n";

    std::shuffle(keys.begin(), keys.end(), g);
    std::cout<<"Keys have been shuffled!\n";
    auto start = std::chrono::high_resolution_clock::now();
    for(int i=0;i<keys.size();i++){
        auto key = keys[i];
        auto it = unsigned_big_int_alex_index.find(key);
        if (it != unsigned_big_int_alex_index.end()) {
            double payload = it.payload();
            sum+=payload;
        }
    }
    std::cout<<"Average : "<<sum/keys.size()<<"\n";
    auto end = std::chrono::high_resolution_clock::now();
    std::chrono::duration<double> elapsed_seconds = end - start;
    std::cout << "Time taken to lookup "<<results.size()<<" keys is "<< elapsed_seconds.count() << " seconds\n";
    std::cout<<"Checking Correctness: \n";
    std::string query = "SELECT AVG(payload) FROM "+table_name+";";

    start = std::chrono::high_resolution_clock::now();
    auto res = con.Query(query);
    if(!res->HasError()){
        res->Print();
    }
    end = std::chrono::high_resolution_clock::now();
    elapsed_seconds = end - start;
    std::cout << "Time taken to avg from DuckDB is "<<results.size()<<" keys is "<< elapsed_seconds.count() << " seconds\n";
}

template<typename K>
void runLookupBenchmarkOneBatchART(duckdb::Connection& con,std::string benchmark_name){
    std::cout<<"Running benchmark with one batch";
    /*
    My rationale here - I will run the benchmark for one batch - read a defined number of keys and count the time needed to do that.
    */

   // Create a random number generator
    std::random_device rd;
    std::mt19937 g(rd());

   vector<double> payloads;
   vector<K>keys;
   std::cout<<"benchmark name "<<benchmark_name<<"\n";
   
   //std::string lookup_query = "SELECT key from "+benchmark_name+" where key = ";
   vector<K>query_keys;
   std::string in_clause = "";
   for(int i=0;i<results.size();i++){
        std::vector<duckdb::unique_ptr<duckdb::Base>>& vec = results.at(i);
        if(typeid(K).name() == typeid(INT64_KEY_TYPE).name()){
            auto key = dynamic_cast<BigIntData*>(vec[0].get())->value;
            query_keys.push_back(key);
        }
        else if(typeid(K).name() == typeid(UNSIGNED_INT64_KEY_TYPE).name()){
            auto key = dynamic_cast<UBigIntData*>(vec[0].get())->value;
            query_keys.push_back(key);
        }
        else{
            auto key = dynamic_cast<DoubleData*>(vec[0].get())->value;
            query_keys.push_back(key);
        }
        
        // std::ostringstream stream;
        // stream << key;
        // std::string ressy = stream.str();
        
    }

    //shuffle the query_key array
    std::shuffle(query_keys.begin(), query_keys.end(), g);

    for(int i=0;i<query_keys.size();i++){
        //std::cout<<"Key "<<query_keys[i]<<"\n";
        if(i!=0){
            in_clause += ",";
        }

        in_clause += std::to_string(query_keys[i]);
    }
    double sum = 0;
    std::unique_ptr<PreparedStatement> prepare = con.Prepare("SELECT payload FROM "+benchmark_name+" WHERE key IN (" + in_clause + ")");
    // for(int i=0;i<payloads.size();i++){
    //     sum += payloads[i];
    // }
    // std::cout<<"Sum of payloads "<<sum<<"\n";
    // std::cout<<"Average "<<sum/payloads.size()<<"\n";


    //std::shuffle(keys.begin(), keys.end(), g);
    std::cout<<"Keys have been shuffled!\n";
    auto start = std::chrono::high_resolution_clock::now();
    std::unique_ptr<QueryResult> res = prepare->Execute();
    if(res->HasError()){
        std::cout<<"Error in query "<<"\n";
    }
    auto end = std::chrono::high_resolution_clock::now();
    std::chrono::duration<double> elapsed_seconds = end - start;
    std::cout << "Time taken to lookup "<<results.size()<<" keys is "<< elapsed_seconds.count() << " seconds\n";
    
}



/*
Run the Benchmarks on different indexes.
*/

template <typename K>
void runLookupBenchmarkAlex(K *keys);

template <>
void runLookupBenchmarkAlex(double *keys){

    if(double_alex_index.size()==0){
        std::cout<<"Index is empty. Please load the data into the index first."<<"\n";
        return;
    }

    std::cout<<"Running benchmark workload "<<"\n";
    int init_num_keys = load_end_point;
    int total_num_keys = 40000;
    int batch_size = 10000;
    double insert_frac = 0.5;
    string lookup_distribution = "zipf";
    int i = init_num_keys;
    long long cumulative_inserts = 0;
    long long cumulative_lookups = 0;
    int num_inserts_per_batch = static_cast<int>(batch_size * insert_frac);
    int num_lookups_per_batch = batch_size - num_inserts_per_batch;
    double cumulative_insert_time = 0;
    double cumulative_lookup_time = 0;
    double time_limit = 0.1;
    bool print_batch_stats = true;
    


    auto workload_start_time = std::chrono::high_resolution_clock::now();
    int batch_no = 0;
    INDEX_PAYLOAD_TYPE sum = 0;
    std::cout << std::scientific;
    std::cout << std::setprecision(3);

    while (true) {
        batch_no++;

        // Do lookups
        double batch_lookup_time = 0.0;
        if (i > 0) {
        double* lookup_keys = nullptr;
        if (lookup_distribution == "uniform") {
            lookup_keys = get_search_keys(keys, i, num_lookups_per_batch);
        } else if (lookup_distribution == "zipf") {
            lookup_keys = get_search_keys_zipf(keys, i, num_lookups_per_batch);
        } else {
            std::cerr << "--lookup_distribution must be either 'uniform' or 'zipf'"
                    << std::endl;
            //return 1;
        }
        auto lookups_start_time = std::chrono::high_resolution_clock::now();
        for (int j = 0; j < num_lookups_per_batch; j++) {
            double key = lookup_keys[j];
            INDEX_PAYLOAD_TYPE* payload = double_alex_index.get_payload(key);
            // std::cout<<"Key "<<key<<" Payload "<<*payload<<"\n";
            if (payload) {
                std::cout<<"Payload is there! "<<"\n";
                sum += *payload;
            }
            else{
                std::cout<<"Payload is not here!! "<<"\n";
            }
        }
        auto lookups_end_time = std::chrono::high_resolution_clock::now();
        batch_lookup_time = std::chrono::duration_cast<std::chrono::nanoseconds>(
                                lookups_end_time - lookups_start_time)
                                .count();
        cumulative_lookup_time += batch_lookup_time;
        cumulative_lookups += num_lookups_per_batch;
        delete[] lookup_keys;
        }
        double workload_elapsed_time =
            std::chrono::duration_cast<std::chrono::nanoseconds>(
                std::chrono::high_resolution_clock::now() - workload_start_time)
                .count();
        if (workload_elapsed_time > time_limit * 1e9 * 60) {
        break;
        }
        if (print_batch_stats) {
        int num_batch_operations = num_lookups_per_batch;
        double batch_time = batch_lookup_time;
        long long cumulative_operations = cumulative_lookups;
        double cumulative_time = cumulative_lookup_time;
        std::cout << "Batch " << batch_no
                    << ", cumulative ops: " << cumulative_operations
                    << "\n\tbatch throughput:\t"
                    << num_lookups_per_batch / batch_lookup_time * 1e9
                    << " lookups/sec,\t"
                    << cumulative_lookups / cumulative_lookup_time * 1e9
                    << " lookups/sec,\t"
                    << cumulative_operations / cumulative_time * 1e9 << " ops/sec"
                    << std::endl;
        }
    }
    long long cumulative_operations = cumulative_lookups + cumulative_inserts;
    double cumulative_time = cumulative_lookup_time + cumulative_insert_time;
    std::cout << "Cumulative stats: " << batch_no << " batches, "
                << cumulative_operations << " ops (" << cumulative_lookups
                << " lookups, " << cumulative_inserts << " inserts)"
                << "\n\tcumulative throughput:\t"
                << cumulative_lookups / cumulative_lookup_time * 1e9
                << " lookups/sec,\t"
                << cumulative_inserts / cumulative_insert_time * 1e9
                << " inserts/sec,\t"
                << cumulative_operations / cumulative_time * 1e9 << " ops/sec"
                << std::endl;

    delete[] keys;
}



template <>
void runLookupBenchmarkAlex(INT64_KEY_TYPE *keys){


    if(big_int_alex_index.size()==0){
        std::cout<<"Index is empty. Please load the data into the index first."<<"\n";
        return;
    }

    std::cout<<"Running benchmark workload "<<"\n";

    int init_num_keys = load_end_point;
    int total_num_keys = 400000;
    int batch_size = 100000;
    double insert_frac = 0.5;
    string lookup_distribution = "zipf";
    int i = init_num_keys;
    long long cumulative_inserts = 0;
    long long cumulative_lookups = 0;
    int num_lookups_per_batch = batch_size;
    double cumulative_lookup_time = 0;
    double time_limit = 0.1;
    bool print_batch_stats = true;
    double elapsed_time_seconds = 0;

    std::cout<<"Num lookups per batch "<<num_lookups_per_batch<<"\n";

    
    auto workload_start_time = std::chrono::high_resolution_clock::now();
    int batch_no = 0;
    INDEX_PAYLOAD_TYPE sum = 0;
    std::cout << std::scientific;
    std::cout << std::setprecision(3);

    while (true) {
        batch_no++;

        // Do lookups
        double batch_lookup_time = 0.0;
        if (i > 0) {
        int64_t* lookup_keys = nullptr;
        if (lookup_distribution == "uniform") {
            lookup_keys = get_search_keys(keys, i, num_lookups_per_batch);
        } else if (lookup_distribution == "zipf") {
            lookup_keys = get_search_keys_zipf(keys, i, num_lookups_per_batch);
        } else {
            std::cerr << "--lookup_distribution must be either 'uniform' or 'zipf'"
                    << std::endl;
            //return 1;
        }
        auto lookups_start_time = std::chrono::high_resolution_clock::now();
        for (int j = 0; j < num_lookups_per_batch; j++) {
            int64_t key = lookup_keys[j];
            INDEX_PAYLOAD_TYPE* payload = big_int_alex_index.get_payload(key);
            //std::cout<<"Key "<<key<<" Payload "<<*payload<<"\n";
            if (payload) {
                //std::cout<<"Payload is there! "<<"\n";
                sum += *payload;
            }
            else{
                std::cout<<"Payload is not here!! "<<"\n";
            }
        }
        auto lookups_end_time = std::chrono::high_resolution_clock::now();

        auto elapsed_seconds = lookups_end_time - lookups_start_time;
        elapsed_time_seconds = elapsed_seconds.count();

        batch_lookup_time = std::chrono::duration_cast<std::chrono::nanoseconds>(
                                lookups_end_time - lookups_start_time)
                                .count();
        cumulative_lookup_time += batch_lookup_time;
        cumulative_lookups += num_lookups_per_batch;
        delete[] lookup_keys;
        }
        double workload_elapsed_time =
            std::chrono::duration_cast<std::chrono::nanoseconds>(
                std::chrono::high_resolution_clock::now() - workload_start_time)
                .count();
        if (workload_elapsed_time > time_limit * 1e9 * 60) {
            break;
        }
        if (print_batch_stats) {
            int num_batch_operations = num_lookups_per_batch;
            double batch_time = batch_lookup_time;
            long long cumulative_operations = cumulative_lookups;
            double cumulative_time = cumulative_lookup_time;
            std::cout << "Batch " << batch_no
                        <<"Batch lookup time "<<elapsed_time_seconds<<"\n"
                        << ", cumulative ops: " << cumulative_operations
                        << "\n\tbatch throughput:\t"
                        << num_lookups_per_batch / batch_lookup_time * 1e9
                        << " lookups/sec,\t"
                        << cumulative_lookups / cumulative_lookup_time * 1e9
                        << " lookups/sec,\t"
                        << cumulative_operations / cumulative_time * 1e9 << " ops/sec"
                        << std::endl;
            }
    }
    // long long avg_lookup_time_per_batch = cumulative_lookups/batch_no;
    // std::cout<<"Average number of lookups per batch "<<avg_lookup_operations_per_batch<<"\n";
    long long cummulative_time = cumulative_lookup_time;
    std::cout<<"Cumulative time "<<cummulative_time<<"\n";
    std::cout<<"Cumulative lookups "<<cumulative_lookups<<"\n";
    std::cout<<"Throughput : "<< cumulative_lookups / cumulative_lookup_time * 1e9<<"ops/sec\n";
    // long long cumulative_operations = cumulative_lookups + cumulative_inserts;
    // double cumulative_time = cumulative_lookup_time + cumulative_insert_time;
    // std::cout << "Cumulative stats: " << batch_no << " batches, "
    //             << cumulative_operations << " ops (" << cumulative_lookups
    //             << " lookups, " << cumulative_inserts << " inserts)"
    //             << "\n\tcumulative throughput:\t"
    //             << cumulative_lookups / cumulative_lookup_time * 1e9
    //             << " lookups/sec,\t"
    //             << cumulative_inserts / cumulative_insert_time * 1e9
    //             << " inserts/sec,\t"
    //             << cumulative_operations / cumulative_time * 1e9 << " ops/sec"
    //             << std::endl;

    delete[] keys;
}

template <typename K>
void runLookupBenchmarkPgm(K *keys);

template <>
void runLookupBenchmarkPgm(double *keys){

    if(double_dynamic_index.size()==0){
        std::cout<<"Index is empty. Please load the data into the index first."<<"\n";
        return;
    }

    std::cout<<"Running benchmark workload "<<"\n";
    int init_num_keys = load_end_point;
    int total_num_keys = 40000;
    int batch_size = 10000;
    double insert_frac = 0.5;
    string lookup_distribution = "zipf";
    int i = init_num_keys;
    long long cumulative_inserts = 0;
    long long cumulative_lookups = 0;
    int num_inserts_per_batch = static_cast<int>(batch_size * insert_frac);
    int num_lookups_per_batch = batch_size - num_inserts_per_batch;
    double cumulative_insert_time = 0;
    double cumulative_lookup_time = 0;
    double time_limit = 0.1;
    bool print_batch_stats = true;
    


    auto workload_start_time = std::chrono::high_resolution_clock::now();
    int batch_no = 0;
    INDEX_PAYLOAD_TYPE sum = 0;
    std::cout << std::scientific;
    std::cout << std::setprecision(3);

    while (true) {
        batch_no++;

        // Do lookups
        double batch_lookup_time = 0.0;
        if (i > 0) {
        double* lookup_keys = nullptr;
        if (lookup_distribution == "uniform") {
            lookup_keys = get_search_keys(keys, i, num_lookups_per_batch);
        } else if (lookup_distribution == "zipf") {
            lookup_keys = get_search_keys_zipf(keys, i, num_lookups_per_batch);
        } else {
            std::cerr << "--lookup_distribution must be either 'uniform' or 'zipf'"
                    << std::endl;
            //return 1;
        }
        auto lookups_start_time = std::chrono::high_resolution_clock::now();
        for (int j = 0; j < num_lookups_per_batch; j++) {
            double key = lookup_keys[j];
            // INDEX_PAYLOAD_TYPE* payload = double_index.get_payload(key); pointer returned
            // INDEX_PAYLOAD_TYPE payload = double_dynamic_index.find(key)->second;
            // std::cout<<"Key "<<key<<" Payload "<<*payload<<"\n";
            if (double_dynamic_index.find(key) != double_dynamic_index.end()) {
                std::cout<<"Payload is there! "<<"\n";
                sum += double_dynamic_index.find(key)->second;
            }
            else{
                std::cout<<"Payload is not here!! "<<"\n";
            }
        }
        auto lookups_end_time = std::chrono::high_resolution_clock::now();
        batch_lookup_time = std::chrono::duration_cast<std::chrono::nanoseconds>(
                                lookups_end_time - lookups_start_time)
                                .count();
        cumulative_lookup_time += batch_lookup_time;
        cumulative_lookups += num_lookups_per_batch;
        delete[] lookup_keys;
        }
        double workload_elapsed_time =
            std::chrono::duration_cast<std::chrono::nanoseconds>(
                std::chrono::high_resolution_clock::now() - workload_start_time)
                .count();
        if (workload_elapsed_time > time_limit * 1e9 * 60) {
        break;
        }
        if (print_batch_stats) {
        int num_batch_operations = num_lookups_per_batch;
        double batch_time = batch_lookup_time;
        long long cumulative_operations = cumulative_lookups;
        double cumulative_time = cumulative_lookup_time;
        std::cout << "Batch " << batch_no
                    << ", cumulative ops: " << cumulative_operations
                    << "\n\tbatch throughput:\t"
                    << num_lookups_per_batch / batch_lookup_time * 1e9
                    << " lookups/sec,\t"
                    << cumulative_lookups / cumulative_lookup_time * 1e9
                    << " lookups/sec,\t"
                    << cumulative_operations / cumulative_time * 1e9 << " ops/sec"
                    << std::endl;
        }
    }
    long long cumulative_operations = cumulative_lookups + cumulative_inserts;
    double cumulative_time = cumulative_lookup_time + cumulative_insert_time;
    std::cout << "Cumulative stats: " << batch_no << " batches, "
                << cumulative_operations << " ops (" << cumulative_lookups
                << " lookups, " << cumulative_inserts << " inserts)"
                << "\n\tcumulative throughput:\t"
                << cumulative_lookups / cumulative_lookup_time * 1e9
                << " lookups/sec,\t"
                << cumulative_inserts / cumulative_insert_time * 1e9
                << " inserts/sec,\t"
                << cumulative_operations / cumulative_time * 1e9 << " ops/sec"
                << std::endl;

    delete[] keys;
}

template <>
void runLookupBenchmarkPgm(INT64_KEY_TYPE *keys){


    if(big_int_dynamic_index.size()==0){
        std::cout<<"Index is empty. Please load the data into the index first."<<"\n";
        return;
    }

    std::cout<<"Running benchmark workload "<<"\n";

    int init_num_keys = load_end_point;
    int total_num_keys = 400000;
    int batch_size = 100000;
    double insert_frac = 0.5;
    string lookup_distribution = "zipf";
    int i = init_num_keys;
    long long cumulative_inserts = 0;
    long long cumulative_lookups = 0;
    int num_lookups_per_batch = batch_size;
    double cumulative_lookup_time = 0;
    double time_limit = 0.1;
    bool print_batch_stats = true;
    double elapsed_time_seconds = 0;

    std::cout<<"Num lookups per batch "<<num_lookups_per_batch<<"\n";

    
    auto workload_start_time = std::chrono::high_resolution_clock::now();
    int batch_no = 0;
    INDEX_PAYLOAD_TYPE sum = 0;
    std::cout << std::scientific;
    std::cout << std::setprecision(3);

    while (true) {
        batch_no++;

        // Do lookups
        double batch_lookup_time = 0.0;
        if (i > 0) {
        int64_t* lookup_keys = nullptr;
        if (lookup_distribution == "uniform") {
            lookup_keys = get_search_keys(keys, i, num_lookups_per_batch);
        } else if (lookup_distribution == "zipf") {
            lookup_keys = get_search_keys_zipf(keys, i, num_lookups_per_batch);
        } else {
            std::cerr << "--lookup_distribution must be either 'uniform' or 'zipf'"
                    << std::endl;
            //return 1;
        }
        auto lookups_start_time = std::chrono::high_resolution_clock::now();
        for (int j = 0; j < num_lookups_per_batch; j++) {
            int64_t key = lookup_keys[j];
            // INDEX_PAYLOAD_TYPE* payload = big_int_dynamic_index.get_payload(key);
            //std::cout<<"Key "<<key<<" Payload "<<*payload<<"\n";
            if (big_int_dynamic_index.find(key) != big_int_dynamic_index.end())  {
                //std::cout<<"Payload is there! "<<"\n";
                sum += big_int_dynamic_index.find(key)->second;
            }
            else{
                std::cout<<"Payload is not here!! "<<"\n";
            }
        }
        auto lookups_end_time = std::chrono::high_resolution_clock::now();

        auto elapsed_seconds = lookups_end_time - lookups_start_time;
        elapsed_time_seconds = elapsed_seconds.count();

        batch_lookup_time = std::chrono::duration_cast<std::chrono::nanoseconds>(
                                lookups_end_time - lookups_start_time)
                                .count();
        cumulative_lookup_time += batch_lookup_time;
        cumulative_lookups += num_lookups_per_batch;
        delete[] lookup_keys;
        }
        double workload_elapsed_time =
            std::chrono::duration_cast<std::chrono::nanoseconds>(
                std::chrono::high_resolution_clock::now() - workload_start_time)
                .count();
        if (workload_elapsed_time > time_limit * 1e9 * 60) {
            break;
        }
        if (print_batch_stats) {
            int num_batch_operations = num_lookups_per_batch;
            double batch_time = batch_lookup_time;
            long long cumulative_operations = cumulative_lookups;
            double cumulative_time = cumulative_lookup_time;
            std::cout << "Batch " << batch_no
                        <<"Batch lookup time "<<elapsed_time_seconds<<"\n"
                        << ", cumulative ops: " << cumulative_operations
                        << "\n\tbatch throughput:\t"
                        << num_lookups_per_batch / batch_lookup_time * 1e9
                        << " lookups/sec,\t"
                        << cumulative_lookups / cumulative_lookup_time * 1e9
                        << " lookups/sec,\t"
                        << cumulative_operations / cumulative_time * 1e9 << " ops/sec"
                        << std::endl;
            }
    }
    // long long avg_lookup_time_per_batch = cumulative_lookups/batch_no;
    // std::cout<<"Average number of lookups per batch "<<avg_lookup_operations_per_batch<<"\n";
    long long cummulative_time = cumulative_lookup_time;
    std::cout<<"Cumulative time "<<cummulative_time<<"\n";
    std::cout<<"Cumulative lookups "<<cumulative_lookups<<"\n";
    std::cout<<"Throughput : "<< cumulative_lookups / cumulative_lookup_time * 1e9<<"ops/sec\n";
    // long long cumulative_operations = cumulative_lookups + cumulative_inserts;
    // double cumulative_time = cumulative_lookup_time + cumulative_insert_time;
    // std::cout << "Cumulative stats: " << batch_no << " batches, "
    //             << cumulative_operations << " ops (" << cumulative_lookups
    //             << " lookups, " << cumulative_inserts << " inserts)"
    //             << "\n\tcumulative throughput:\t"
    //             << cumulative_lookups / cumulative_lookup_time * 1e9
    //             << " lookups/sec,\t"
    //             << cumulative_inserts / cumulative_insert_time * 1e9
    //             << " inserts/sec,\t"
    //             << cumulative_operations / cumulative_time * 1e9 << " ops/sec"
    //             << std::endl;

    delete[] keys;
}



void functionRunLookupBenchmark(ClientContext &context, const FunctionParameters &parameters){
    std::cout<<"Running lookup benchmark"<<"\n";
    std::string benchmarkName = parameters.values[0].GetValue<string>();
    std::string index = parameters.values[1].GetValue<string>();
    duckdb::Connection con(*context.db);

    std::string keys_file_path = "";
    if(benchmarkName == "lognormal"){
        keys_file_path = "/Users/jishnusm/Desktop/classes/AdvancedDataStores/Project/Project2/radix/test/lognormal-190M.bin";
        auto keys = new INT64_KEY_TYPE[load_end_point];
        std::cout<<"Loading binary data "<<std::endl;
        load_binary_data(keys, load_end_point, keys_file_path);
        if(index == "alex"){
            runLookupBenchmarkAlex<INT64_KEY_TYPE>(keys);
        }
        else if(index=="pgm"){
            runLookupBenchmarkPgm<INT64_KEY_TYPE>(keys);
        }
        
        // else{
        //     runLookupBenchmarkArt<INT64_KEY_TYPE>(keys,con,benchmarkName);
        // }
    }
    else if(benchmarkName == "longlat"){
        keys_file_path = "/Users/jishnusm/Desktop/classes/AdvancedDataStores/Project/Project2/radix/test/longlat-200M.bin";
        auto keys = new DOUBLE_KEY_TYPE[load_end_point];
        std::cout<<"Loading binary data "<<std::endl;
        load_binary_data(keys, load_end_point, keys_file_path);
        if(index == "alex"){
            runLookupBenchmarkAlex<DOUBLE_KEY_TYPE>(keys);
        }
        else if(index=="pgm"){
            runLookupBenchmarkPgm<DOUBLE_KEY_TYPE>(keys);
        }
        // else{
        //     runLookupBenchmarkArt<DOUBLE_KEY_TYPE>(keys,con,benchmarkName);
        // }
    }
    else if(benchmarkName=="ycsb"){
        keys_file_path = "/Users/jishnusm/Desktop/classes/AdvancedDataStores/Project/Project2/radix/test/ycsb-200M.bin";
        auto keys = new INT64_KEY_TYPE[load_end_point];
        std::cout<<"Loading binary data "<<std::endl;
        load_binary_data(keys, load_end_point, keys_file_path);
        if(index == "alex"){
            runLookupBenchmarkAlex<INT64_KEY_TYPE>(keys);
        }
        else if(index=="pgm"){
            runLookupBenchmarkPgm<INT64_KEY_TYPE>(keys);
        }
        // else{
        //     runLookupBenchmarkArt<INT64_KEY_TYPE>(keys,con,benchmarkName);
        // }
    }
    else if(benchmarkName == "longitudes"){
        keys_file_path = "/Users/jishnusm/Desktop/classes/AdvancedDataStores/Project/Project2/radix/test/longitudes-200M.bin";
        auto keys = new DOUBLE_KEY_TYPE[load_end_point];
        std::cout<<"Loading binary data "<<std::endl;
        load_binary_data(keys, load_end_point, keys_file_path); 
        if(index == "alex"){
            runLookupBenchmarkAlex<DOUBLE_KEY_TYPE>(keys);
        }
        else if(index=="pgm"){
            runLookupBenchmarkPgm<DOUBLE_KEY_TYPE>(keys);
        }
        // else{
        //     runLookupBenchmarkArt<DOUBLE_KEY_TYPE>(keys,con,benchmarkName);
        // }
    }

}


template <typename K>
void print_stats(){
    if(typeid(K)==typeid(DOUBLE_KEY_TYPE)){
        auto stats = double_alex_index.get_stats();
        std::cout<<"Stats about the index \n";
        std::cout<<"***************************\n";
        std::cout<<"Number of keys : "<<stats.num_keys<<"\n";
        std::cout<<"Number of model nodes : "<<stats.num_model_nodes<<"\n";
        std::cout<<"Number of data nodes: "<<stats.num_data_nodes<<"\n";
    }
    else if(typeid(K)==typeid(UNSIGNED_INT64_KEY_TYPE)){
        auto stats = unsigned_big_int_alex_index.get_stats();
        std::cout<<"Stats about the index \n";
        std::cout<<"***************************\n";
        std::cout<<"Number of keys : "<<stats.num_keys<<"\n";
        std::cout<<"Number of model nodes : "<<stats.num_model_nodes<<"\n";
        std::cout<<"Number of data nodes: "<<stats.num_data_nodes<<"\n";
    }
    else if(typeid(K)==typeid(INT_KEY_TYPE)){
        auto stats = int_alex_index.get_stats();
        std::cout<<"Stats about the index \n";
        std::cout<<"***************************\n";
        std::cout<<"Number of keys : "<<stats.num_keys<<"\n";
        std::cout<<"Number of model nodes : "<<stats.num_model_nodes<<"\n";
        std::cout<<"Number of data nodes: "<<stats.num_data_nodes<<"\n";
    }
    else{
        auto stats = big_int_alex_index.get_stats();
        std::cout<<"Stats about the index \n";
        std::cout<<"***************************\n";
        std::cout<<"Number of keys : "<<stats.num_keys<<"\n";
        std::cout<<"Number of model nodes : "<<stats.num_model_nodes<<"\n";
        std::cout<<"Number of data nodes: "<<stats.num_data_nodes<<"\n";
    }

}

/*
Bulk Load into Index functions
*/

template <typename K,typename P>
void bulkLoadIntoIndex(duckdb::Connection & con,std::string table_name,int column_index){
    std::cout<<"General Function with no consequence.\n"; 
}

template<>
void bulkLoadIntoIndex<double,INDEX_PAYLOAD_TYPE>(duckdb::Connection & con,std::string table_name,int column_index){
/*
    Phase 1: Load the data from the table.
    */
    string query = "SELECT * FROM "+table_name+";";
    unique_ptr<MaterializedQueryResult> result = con.Query(query);
    results = result->getContents();
    int num_keys = results.size();
    //std::cout<<"Num Keys : "<<num_keys<<"\n";

   /*
    Phase 2: Bulk load the data from the results vector into the pair array that goes into the index.
   */
   std::pair<double,INDEX_PAYLOAD_TYPE>* bulk_load_values = new std::pair<double,INDEX_PAYLOAD_TYPE>[num_keys];  
    int max_key = INT_MIN;
    for (int i=0;i<results.size();i++){
        int row_id = i;
        //std::cout<<"before key"<<"\n";
        auto rrr = results[i][column_index].get();
        
        double key_ = dynamic_cast<DoubleData*>(rrr)->value;
        double value_ = dynamic_cast<DoubleData*>(results[i][column_index+1].get())->value;
        
        //std::cout<<"after key"<<"\n";
        bulk_load_values[i] = {key_,value_};
    }
    /**
     Phase 3: Sort the bulk load values array based on the key values.
    */

    auto start_time = std::chrono::high_resolution_clock::now();
    std::sort(bulk_load_values,bulk_load_values+num_keys,[](auto const& a, auto const& b) { return a.first < b.first; });

    /*
    Phase 4: Bulk load the sorted values into the index.
    */
    
    double_alex_index.bulk_load(bulk_load_values, num_keys);
    auto end_time = std::chrono::high_resolution_clock::now();
    std::chrono::duration<double> elapsed_seconds = end_time - start_time;
    std::cout << "Time taken to bulk load: " << elapsed_seconds.count() << " seconds\n\n\n";
    print_stats<DOUBLE_KEY_TYPE>();
}

template<>
void bulkLoadIntoIndex<int64_t,INDEX_PAYLOAD_TYPE>(duckdb::Connection & con,std::string table_name,int column_index){
/*
    Phase 1: Load the data from the table.
    */
    string query = "SELECT * FROM "+table_name+";";
    unique_ptr<MaterializedQueryResult> result = con.Query(query);
    results = result->getContents();
    int num_keys = results.size();
    //std::cout<<"Num Keys : "<<num_keys<<"\n";

   /*
    Phase 2: Bulk load the data from the results vector into the pair array that goes into the index.
   */
   std::pair<int64_t,INDEX_PAYLOAD_TYPE>* bulk_load_values = new std::pair<int64_t,INDEX_PAYLOAD_TYPE>[num_keys];
    //std::cout<<"Col index "<<column_index<<"\n";    
    int max_key = INT_MIN;
    for (int i=0;i<results.size();i++){
        int row_id = i;
        //std::cout<<"before key"<<"\n";
        auto rrr = results[i][column_index].get();
        
        int64_t key_ = dynamic_cast<BigIntData*>(rrr)->value;
        double value_ = dynamic_cast<DoubleData*>(results[i][column_index+1].get())->value;
        
        //std::cout<<"after key"<<"\n";
        bulk_load_values[i] = {key_,value_};
    }
    /**
     Phase 3: Sort the bulk load values array based on the key values.
    */
   //Measure time 
    
    auto start_time = std::chrono::high_resolution_clock::now();
    std::sort(bulk_load_values,bulk_load_values+num_keys,[](auto const& a, auto const& b) { return a.first < b.first; });
    
    /*
    Phase 4: Bulk load the sorted values into the index.
    */
    
    big_int_alex_index.bulk_load(bulk_load_values, num_keys);
    auto end_time = std::chrono::high_resolution_clock::now();
    std::chrono::duration<double> elapsed_seconds = end_time - start_time;
    std::cout << "Time taken to bulk load: " << elapsed_seconds.count() <<" seconds\n\n\n";
    print_stats<INT64_KEY_TYPE>();
}

template<>
void bulkLoadIntoIndex<UNSIGNED_INT64_KEY_TYPE,INDEX_PAYLOAD_TYPE>(duckdb::Connection & con,std::string table_name,int column_index){
/*
    Phase 1: Load the data from the table.
    */
    string query = "SELECT * FROM "+table_name+";";
    unique_ptr<MaterializedQueryResult> result = con.Query(query);
    results = result->getContents();
    int num_keys = results.size();
    //std::cout<<"Num Keys : "<<num_keys<<"\n";

   /*
    Phase 2: Bulk load the data from the results vector into the pair array that goes into the index.
   */
   std::pair<UNSIGNED_INT64_KEY_TYPE,INDEX_PAYLOAD_TYPE>* bulk_load_values = new std::pair<UNSIGNED_INT64_KEY_TYPE,INDEX_PAYLOAD_TYPE>[num_keys];
    //std::cout<<"Col index "<<column_index<<"\n";    
    int max_key = INT_MIN;
    for (int i=0;i<results.size();i++){
        int row_id = i;
        //std::cout<<"before key"<<"\n";
        auto rrr = results[i][column_index].get();
        
        UNSIGNED_INT64_KEY_TYPE key_ = dynamic_cast<UBigIntData*>(rrr)->value;
        double value_ = dynamic_cast<DoubleData*>(results[i][column_index+1].get())->value;
        
        //std::cout<<"after key"<<"\n";
        bulk_load_values[i] = {key_,value_};
    }
    /**
     Phase 3: Sort the bulk load values array based on the key values.
    */
   //Measure time 
    
    auto start_time = std::chrono::high_resolution_clock::now();
    std::sort(bulk_load_values,bulk_load_values+num_keys,[](auto const& a, auto const& b) { return a.first < b.first; });
    
    /*
    Phase 4: Bulk load the sorted values into the index.
    */
    
    unsigned_big_int_alex_index.bulk_load(bulk_load_values, num_keys);
    auto end_time = std::chrono::high_resolution_clock::now();
    std::chrono::duration<double> elapsed_seconds = end_time - start_time;
    std::cout << "Time taken to bulk load: " << elapsed_seconds.count() <<" seconds\n\n\n";
    print_stats<UNSIGNED_INT64_KEY_TYPE>();
}

template<>
void bulkLoadIntoIndex<INT_KEY_TYPE,INDEX_PAYLOAD_TYPE>(duckdb::Connection & con,std::string table_name,int column_index){
/*
    Phase 1: Load the data from the table.
    */
    string query = "SELECT * FROM "+table_name+";";
    unique_ptr<MaterializedQueryResult> result = con.Query(query);
    results = result->getContents();
    int num_keys = results.size();
    //std::cout<<"Num Keys : "<<num_keys<<"\n";

   /*
    Phase 2: Bulk load the data from the results vector into the pair array that goes into the index.
   */
   std::pair<INT_KEY_TYPE,INDEX_PAYLOAD_TYPE>* bulk_load_values = new std::pair<INT_KEY_TYPE,INDEX_PAYLOAD_TYPE>[num_keys];
    //std::cout<<"Col index "<<column_index<<"\n";    
    int max_key = INT_MIN;
    for (int i=0;i<results.size();i++){
        int row_id = i;
        //std::cout<<"before key"<<"\n";
        auto rrr = results[i][column_index].get();
        
        INT_KEY_TYPE key_ = dynamic_cast<IntData*>(rrr)->value;
        double value_ = dynamic_cast<DoubleData*>(results[i][column_index+1].get())->value;
        
        //std::cout<<"after key"<<"\n";
        bulk_load_values[i] = {key_,i};
    }
    /**
     Phase 3: Sort the bulk load values array based on the key values.
    */
   //Measure time 
    
    auto start_time = std::chrono::high_resolution_clock::now();
    std::sort(bulk_load_values,bulk_load_values+num_keys,[](auto const& a, auto const& b) { return a.first < b.first; });
    
    /*
    Phase 4: Bulk load the sorted values into the index.
    */
    
    int_alex_index.bulk_load(bulk_load_values, num_keys);
    auto end_time = std::chrono::high_resolution_clock::now();
    std::chrono::duration<double> elapsed_seconds = end_time - start_time;
    std::cout << "Time taken to bulk load: " << elapsed_seconds.count() <<" seconds\n\n\n";
    print_stats<INT_KEY_TYPE>();
}

/**
 * Index Creation
 * 
*/

void functionCreateARTIndex(ClientContext &context, const FunctionParameters &parameters){

    std::string table_name = parameters.values[0].GetValue<string>();
    std::string column_name = parameters.values[1].GetValue<string>();

    QualifiedName qname = GetQualifiedName(context, table_name);
    //CheckIfTableExists(context, qname);
    auto &table = Catalog::GetEntry<TableCatalogEntry>(context, qname.catalog, qname.schema, qname.name);
    auto &columnList = table.GetColumns();

    vector<string>columnNames = columnList.GetColumnNames();
    for(int i=0;i<columnNames.size();i++){
        std::cout<<"Column name "<<columnNames[i]<<"\n";
        if(column_name == columnNames[i]){
            std::cout<<"Column name found "<<"\n";
            duckdb::Connection con(*context.db);
            std::cout<<"Creating an ART index for this column"<<"\n";
            string query = "CREATE INDEX "+column_name+"_art_index ON "+table_name+"("+column_name+");";
            //Measure time 
            auto start_time = std::chrono::high_resolution_clock::now();
            auto result = con.Query(query);
            auto end_time = std::chrono::high_resolution_clock::now();
            std::chrono::duration<double> elapsed_seconds = end_time - start_time;
            if(!result->HasError()){
                std::cout<<"Index created successfully "<<"\n";
            }
            else{
                std::cout<<"Index creation failed "<<"\n";
            }
            // Print the time taken to execute the query
            std::cout << "Time taken to execute the query: " << elapsed_seconds.count() << " seconds\n";
        }
        else{
            std::cout<<"Column name not found "<<"\n";
        }
    }
}

void createAlexIndexPragmaFunction(ClientContext &context, const FunctionParameters &parameters){
    string table_name = parameters.values[0].GetValue<string>();
    string column_name = parameters.values[1].GetValue<string>();

    QualifiedName qname = GetQualifiedName(context, table_name);
    //CheckIfTableExists(context, qname);
    auto &table = Catalog::GetEntry<TableCatalogEntry>(context, qname.catalog, qname.schema, qname.name);
    auto &columnList = table.GetColumns(); 

    vector<string>columnNames = columnList.GetColumnNames();
    vector<LogicalType>columnTypes = columnList.GetColumnTypes();
    int count = 0;
    int column_index = -1;
    LogicalType column_type;
    int col_i = 0;
    for(col_i=0;col_i<columnNames.size();col_i++){
        string curr_col_name = columnNames[col_i];
        LogicalType curr_col_type = columnTypes[col_i];
        if(curr_col_name == column_name){
            column_index = count;
            column_type = curr_col_type;
        }
        count++;
    }

    if(column_index == -1){
        std::cout<<"Column not found "<<"\n";
    }
    else{
        duckdb::Connection con(*context.db);
        // std::cout<<"Column found at index "<<column_index<<"\n";
        // std::cout<<"Creating an alex index for this column"<<"\n";
        // std::cout<<"Column Type "<<typeid(column_type).name()<<"\n";
        // std::cout<<"Column Type "<<typeid(double).name()<<"\n";
        // std::cout<<"Column type to string "<<column_type.ToString()<<"\n";
        std::string columnTypeName = column_type.ToString();
        if(columnTypeName == "DOUBLE"){
            bulkLoadIntoIndex<DOUBLE_KEY_TYPE,INDEX_PAYLOAD_TYPE>(con,table_name,column_index);
            index_type_table_name_map.insert({"double",{table_name,column_name}});
        }
        else if(columnTypeName == "BIGINT"){
            bulkLoadIntoIndex<INT64_KEY_TYPE,INDEX_PAYLOAD_TYPE>(con,table_name,column_index);
            index_type_table_name_map.insert({"bigint",{table_name,column_name}});
        }
        else if(columnTypeName == "UBIGINT"){
            bulkLoadIntoIndex<UNSIGNED_INT64_KEY_TYPE,INDEX_PAYLOAD_TYPE>(con,table_name,column_index);
            index_type_table_name_map.insert({"ubigint",{table_name,column_name}});
        }
        else if(columnTypeName == "INTEGER"){
            bulkLoadIntoIndex<INT_KEY_TYPE,INDEX_PAYLOAD_TYPE>(con,table_name,column_index);
            index_type_table_name_map.insert({"int",{table_name,column_name}});
        }
        else{
            std::cout<<"Unsupported column type for alex indexing (for now) "<<"\n";
        }
        //bulkLoadIntoIndex<typeid(column_type).name(),INDEX_PAYLOAD_TYPE>(con,table_name,column_index);
    }
}

template<typename K>
void functionInsertIntoTableAndIndex(duckdb::Connection &con,std::string table_name,K key,DOUBLE_KEY_TYPE value){
    std::cout<<"General template function \n";
}

template<>
void functionInsertIntoTableAndIndex<DOUBLE_KEY_TYPE>(duckdb::Connection &con,std::string table_name,DOUBLE_KEY_TYPE key,DOUBLE_KEY_TYPE value){
    //std::cout<<"Insert into table and index for double key type"<<"\n";
    // std::string query = "INSERT INTO "+table_name+" VALUES(";
    // query+=std::to_string(key)+","+std::to_string(value)+");";

    std::string query = "INSERT INTO " + table_name + " VALUES (?, ?)";
    auto result = con.Query(query, key, value);
    if(!result->HasError()){
        //std::cout<<"Insertion successful "<<"\n";
        if(double_alex_index.size()>0){
            std::vector<unique_ptr<Base>> dataVector;
            dataVector.push_back(make_uniq<DoubleData>(key));
            dataVector.push_back(make_uniq<DoubleData>(value));
            results.push_back(std::move(dataVector));
            double_alex_index.insert({key,value});
        }
        else{
            std::cout<<"Index is empty. So not updating it."<<"\n";
        }
    }
    else{
        std::cout<<"Insertion failed "<<"\n";
    }
}

template<>
void functionInsertIntoTableAndIndex<INT64_KEY_TYPE>(duckdb::Connection &con,std::string table_name,INT64_KEY_TYPE key,DOUBLE_KEY_TYPE value){
    std::cout<<"Insert into table and index for double key type"<<"\n";
    // std::string query = "INSERT INTO "+table_name+" VALUES(";
    // query+=std::to_string(key)+","+std::to_string(value)+");";

    std::string query = "INSERT INTO " + table_name + " VALUES (?, ?)";
    auto result = con.Query(query, key, value);
    if(!result->HasError()){
        std::cout<<"Insertion successful "<<"\n";
        if(big_int_alex_index.size()>0){
            std::vector<unique_ptr<Base>> dataVector;
            dataVector.push_back(make_uniq<BigIntData>(key));
            dataVector.push_back(make_uniq<BigIntData>(value));
            results.push_back(std::move(dataVector));
            big_int_alex_index.insert({key,value});
        }
        else{
            std::cout<<"Index is empty. So not updating it."<<"\n";
        }
    }
    else{
        std::cout<<"Insertion failed "<<"\n";
    }
}

template<>
void functionInsertIntoTableAndIndex<UNSIGNED_INT64_KEY_TYPE>(duckdb::Connection &con,std::string table_name,UNSIGNED_INT64_KEY_TYPE key,DOUBLE_KEY_TYPE value){
    // std::string query = "INSERT INTO "+table_name+" VALUES(";
    // query+=std::to_string(key)+","+std::to_string(value)+");";

    std::string query = "INSERT INTO " + table_name + " VALUES (?, ?)";
    auto result = con.Query(query, key, value);
    if(!result->HasError()){
        std::cout<<"Insertion successful "<<"\n";
        if(unsigned_big_int_alex_index.size()>0){
            std::vector<unique_ptr<Base>> dataVector;
            dataVector.push_back(make_uniq<UBigIntData>(key));
            dataVector.push_back(make_uniq<UBigIntData>(value));
            results.push_back(std::move(dataVector));
            unsigned_big_int_alex_index.insert({key,value});
        }
        else{
            std::cout<<"Index is empty. So not updating it."<<"\n";
        }
    }
    else{
        std::cout<<"Insertion failed "<<"\n";
    }
}

template<>
void functionInsertIntoTableAndIndex<int>(duckdb::Connection &con,std::string table_name,int key,DOUBLE_KEY_TYPE value){
    // std::string query = "INSERT INTO "+table_name+" VALUES(";
    // query+=std::to_string(key)+","+std::to_string(value)+");";

    std::string query = "INSERT INTO " + table_name + " VALUES (?, ?)";
    auto result = con.Query(query, key, value);
    if(!result->HasError()){
        std::cout<<"Insertion successful "<<"\n";
        if(int_alex_index.size()>0){
            std::vector<unique_ptr<Base>> dataVector;
            dataVector.push_back(make_uniq<IntData>(key));
            dataVector.push_back(make_uniq<IntData>(value));
            results.push_back(std::move(dataVector));
            int_alex_index.insert({key,results.size()-1});
        }
        else{
            std::cout<<"Index is empty. So not updating it."<<"\n";
        }
    }
    else{
        std::cout<<"Insertion failed "<<"\n";
    }
}

void functionInsertIntoTable(ClientContext &context, const FunctionParameters &parameters){
    std::string table_name = parameters.values[0].GetValue<string>();
    std::string key_type = parameters.values[1].GetValue<string>();
    std::string key = parameters.values[2].GetValue<string>();
    std::string value = parameters.values[3].GetValue<string>();
    duckdb::Connection con(*context.db);
    if(key_type=="double"){
        double dkey = std::stod(key);
        double dvalue = std::stod(value);
        functionInsertIntoTableAndIndex<double>(con,table_name,dkey,dvalue);
    }
    else if(key_type=="bigint"){
        INT64_KEY_TYPE bkey = std::stoll(key);
        double bvalue = std::stod(value);
        functionInsertIntoTableAndIndex<INT64_KEY_TYPE>(con,table_name,bkey,bvalue);
    }
    else if(key_type =="int"){
        int ikey = std::stoi(key);
        double ivalue = std::stod(value);
        functionInsertIntoTableAndIndex<int>(con,table_name,ikey,ivalue);
    }
    else{
        UNSIGNED_INT64_KEY_TYPE ukey = std::stoull(key);
        double uvalue = std::stod(value);
        functionInsertIntoTableAndIndex<UNSIGNED_INT64_KEY_TYPE>(con,table_name,ukey,uvalue);
    }
    
    //For double index:
}

void functionRunBenchmarkOneBatch(ClientContext &context, const FunctionParameters &parameters){
    std::string benchmark_name = parameters.values[0].GetValue<string>();
    std::string index = parameters.values[1].GetValue<string>();
    std::string table_name = benchmark_name+"_benchmark";
    std::string data_type = parameters.values[2].GetValue<string>();
    duckdb::Connection con(*context.db);
    if(index == "alex"){
        if(data_type == "double"){
            runLookupBenchmarkOneBatchAlex<double>(con,table_name);
        }
        else if(data_type=="bigint"){
            runLookupBenchmarkOneBatchAlex<int64_t>(con,table_name);
        }
        else{
            runLookupBenchmarkOneBatchAlex<uint64_t>(con,table_name);
        }
    }
    else{
        if(data_type == "double"){
            runLookupBenchmarkOneBatchART<double>(con,table_name);
        }
        else if(data_type=="bigint"){
            runLookupBenchmarkOneBatchART<int64_t>(con,table_name);
        }
        else{
            runLookupBenchmarkOneBatchART<uint64_t>(con,table_name);
        }
    }

}

template<typename K>
void runInsertionBenchmarkWorkload(duckdb::Connection& con,std::string benchmarkName,std::string table_name,std::string data_type, int to_insert){
    /**
     * Load the keys into a vector based on the data_type
     * 
    */
    std::string benchmarkFile = "";
    std::string benchmarkFileType = "";

    if(benchmarkName.compare("lognormal")==0){
        benchmarkFile = "/Users/maitrithaker/Desktop/proj/intelligent-duck/lognormal-190M.bin";
        benchmarkFileType = "binary";
    }
    else if(benchmarkName.compare("longitudes")==0){
        benchmarkFile = "/Users/maitrithaker/Desktop/proj/intelligent-duck/longitudes-200M.bin";
        benchmarkFileType = "binary";
    }
    else if(benchmarkName.compare("longlat")==0){
        benchmarkFile = "/Users/maitrithaker/Desktop/proj/intelligent-duck/longlat-200M.bin";
        benchmarkFileType = "binary";
    }
    else if(benchmarkName.compare("ycsb")==0){
        benchmarkFile = "/Users/maitrithaker/Desktop/proj/intelligent-duck/ycsb-200M.bin";
        benchmarkFileType = "binary";
    }

    int new_key_count = load_end_point + to_insert;
    std::cout<<"New key count "<<new_key_count<<"\n";
    auto keys = new K[new_key_count];
    std::string keys_file_type = "binary";
    if (keys_file_type == "binary") {
        std::cout<<"Loading binary data "<<std::endl;
        load_binary_data(keys, new_key_count, benchmarkFile);
    } else if (keys_file_type == "text") {
        load_text_data(keys, new_key_count, benchmarkFile);
    } else {
        std::cerr << "--keys_file_type must be either 'binary' or 'text'"
                << std::endl;
    }

    
    auto values = new std::pair<K, double>[to_insert];
    std::mt19937_64 gen_payload(std::random_device{}());


    for (int vti = 0; vti < to_insert; vti++) {
        //values[vti].first = keys[vti];
        K key = keys[vti+load_end_point];
        //std::cout<<key<<"\n";
        double random_payload = static_cast<double>(gen_payload());
        values[vti] = {key,random_payload};
    }

    auto start_time = std::chrono::high_resolution_clock::now();
    for(int i=0;i<to_insert;i++){
        K key = values[i].first;
        double value = values[i].second;
        if(data_type == "double"){
            functionInsertIntoTableAndIndex<double>(con,table_name,key,value);
        }
        else if(data_type == "bigint"){
            functionInsertIntoTableAndIndex<int64_t>(con,table_name,key,value);
        }
        else{
            functionInsertIntoTableAndIndex<uint64_t>(con,table_name,key,value);
        }
    }
    load_end_point = results.size();
    auto end_time = std::chrono::high_resolution_clock::now();
    std::chrono::duration<double> elapsed_seconds = end_time - start_time;
    std::cout<<"Time taken to insert "<<to_insert<<" keys" << elapsed_seconds.count() << " seconds\n";
}

template<typename K>
void runInsertionBenchmarkWorkloadART(duckdb::Connection& con,std::string benchmarkName,std::string table_name,std::string data_type, int to_insert){
    /**
     * Load the keys into a vector based on the data_type
     * 
    */
    std::string benchmarkFile = "";
    std::string benchmarkFileType = "";

    if(benchmarkName.compare("lognormal")==0){
        benchmarkFile = "/Users/maitrithaker/Desktop/proj/intelligent-duck/lognormal-190M.bin";
        benchmarkFileType = "binary";
    }
    else if(benchmarkName.compare("longitudes")==0){
        benchmarkFile = "/Users/maitrithaker/Desktop/proj/intelligent-duck/longitudes-200M.bin";
        benchmarkFileType = "binary";
    }
    else if(benchmarkName.compare("longlat")==0){
        benchmarkFile = "/Users/maitrithaker/Desktop/proj/intelligent-duck/longlat-200M.bin";
        benchmarkFileType = "binary";
    }
    else if(benchmarkName.compare("ycsb")==0){
        benchmarkFile = "/Users/maitrithaker/Desktop/proj/intelligent-duck/ycsb-200M.bin";
        benchmarkFileType = "binary";
    }

    int new_key_count = load_end_point + to_insert;
    std::cout<<"New key count "<<new_key_count<<"\n";
    auto keys = new K[new_key_count];
    std::string keys_file_type = "binary";
    if (keys_file_type == "binary") {
        std::cout<<"Loading binary data "<<std::endl;
        load_binary_data(keys, new_key_count, benchmarkFile);
    } else if (keys_file_type == "text") {
        load_text_data(keys, new_key_count, benchmarkFile);
    } else {
        std::cerr << "--keys_file_type must be either 'binary' or 'text'"
                << std::endl;
    }

    
    auto values = new std::pair<K, double>[to_insert];
    std::mt19937_64 gen_payload(std::random_device{}());


    for (int vti = 0; vti < to_insert; vti++) {
        //values[vti].first = keys[vti];
        K key = keys[vti+load_end_point];
        //std::cout<<key<<"\n";
        double random_payload = static_cast<double>(gen_payload());
        values[vti] = {key,random_payload};
    }
    std::string query = "INSERT INTO " + table_name + " VALUES (?, ?)";
    
    auto start_time = std::chrono::high_resolution_clock::now();
    for(int i=0;i<to_insert;i++){
        K key = values[i].first;
        double value = values[i].second;
        // if(data_type == "double"){
        //     functionInsertIntoTableAndIndex<double>(con,table_name,key,value);
        // }
        // else if(data_type == "bigint"){
        //     functionInsertIntoTableAndIndex<int64_t>(con,table_name,key,value);
        // }
        // else{
        //     functionInsertIntoTableAndIndex<uint64_t>(con,table_name,key,value);
        // }
        auto result = con.Query(query, key, value);
        if(result->HasError()){
            std::cout<<"Insertion failed "<<"\n";
        }
    }
    load_end_point = results.size();
    auto end_time = std::chrono::high_resolution_clock::now();
    std::chrono::duration<double> elapsed_seconds = end_time - start_time;
    std::cout<<"Time taken to insert "<<to_insert<<" keys" << elapsed_seconds.count() << " seconds\n";

}

void functionRunInsertionBenchmark(ClientContext &context, const FunctionParameters &parameters){
    std::string benchmark_name = parameters.values[0].GetValue<string>();
    std::string table_name = benchmark_name+"_benchmark";
    std::string data_type = parameters.values[1].GetValue<string>();
    std::string index = parameters.values[2].GetValue<string>();
    int to_insert = parameters.values[3].GetValue<int>();


    duckdb::Connection con(*context.db);

    int init_num_keys = load_end_point;

    if(index == "alex"){
        if(data_type=="double"){
            runInsertionBenchmarkWorkload<double>(con,benchmark_name,table_name,data_type,to_insert);
        }
        else if(data_type=="bigint"){
            runInsertionBenchmarkWorkload<int64_t>(con,benchmark_name,table_name,data_type,to_insert);
        }
        else{
            runInsertionBenchmarkWorkload<uint64_t>(con,benchmark_name,table_name,data_type,to_insert);
        }
    }
    else{
        if(data_type=="double"){
            runInsertionBenchmarkWorkloadART<double>(con,benchmark_name,table_name,data_type,to_insert);
        }
        else if(data_type=="bigint"){
            runInsertionBenchmarkWorkloadART<int64_t>(con,benchmark_name,table_name,data_type,to_insert);
        }
        else{
            runInsertionBenchmarkWorkloadART<uint64_t>(con,benchmark_name,table_name,data_type,to_insert);
        }
    }
}

void functionAlexFind(ClientContext &context, const FunctionParameters &parameters){
    std::string index_type = parameters.values[0].GetValue<string>();
    std::string key = parameters.values[1].GetValue<string>();

    if(index_type == "double"){
        double key_ = std::stod(key);
        auto time_start = std::chrono::high_resolution_clock::now();
        auto payload = double_alex_index.get_payload(key_);
        auto time_end = std::chrono::high_resolution_clock::now();
        if(payload){
            std::chrono::duration<double> elapsed_seconds = time_end - time_start;
            std::cout<<"Payload found "<<*payload<<"\n";
            std::cout<<"\nTime taken : "<< elapsed_seconds.count()<<" seconds \n";
        }
        else{
            std::cout<<"Payload not found "<<"\n";
        }
    }
    else if(index_type=="bigint"){
        int64_t key_ = std::stoll(key);
        auto time_start = std::chrono::high_resolution_clock::now();
        auto payload = big_int_alex_index.get_payload(key_);
        auto time_end = std::chrono::high_resolution_clock::now();
        if(payload){
            std::chrono::duration<double> elapsed_seconds = time_end - time_start;
            std::cout<<"Payload found "<<*payload<<"\n";
            std::cout<<"\nTime taken : "<< elapsed_seconds.count()<<" seconds \n";
        }
        else{
            std::cout<<"Payload not found "<<"\n";
        }
    }
    else if(index_type=="int"){
        int key_ = std::stoi(key);
        auto time_start = std::chrono::high_resolution_clock::now();
        auto payload = int_alex_index.get_payload(key_);
        auto time_end = std::chrono::high_resolution_clock::now();
        if(payload){
            std::cout<<"Payload found \n";
            pair<string,string>tab_col = index_type_table_name_map["int"];
            std::string table_name = tab_col.first;
            QualifiedName name = GetQualifiedName(context,table_name);
            auto &table = Catalog::GetEntry<TableCatalogEntry>(context, name.catalog, name.schema, name.name);
            auto &columnList = table.GetColumns();
            vector<string>columnNames = columnList.GetColumnNames();
            display_row(*payload,columnNames);
            std::chrono::duration<double> elapsed_seconds = time_end - time_start;
            std::cout<<"\nTime taken : "<< elapsed_seconds.count()<<" seconds \n";
        }
        else{
            std::cout<<"Key not found!\n";
        }
        std::cout<<"\n";
    }
    else{
        uint64_t key_ = std::stoull(key);
        auto time_start = std::chrono::high_resolution_clock::now();
        auto payload = unsigned_big_int_alex_index.get_payload(key_);
        auto time_end = std::chrono::high_resolution_clock::now();
        if(payload){
            std::cout<<"Payload found "<<*payload<<"\n";
            std::chrono::duration<double> elapsed_seconds = time_end - time_start;
            std::cout<<"\nTime taken : "<< elapsed_seconds.count()<<" seconds \n";
        }
        else{
            std::cout<<"Payload not found "<<"\n";
        }
    }
}

void functionAlexSize(ClientContext &context, const FunctionParameters &parameters){
    std::string index_type = parameters.values[0].GetValue<string>();
    long long total_size = 0;
    long long model_size = 0;
    long long data_size = 0;
    if(index_type == "double"){
        model_size = double_alex_index.model_size();
        data_size = double_alex_index.data_size();
        //std::cout<<"Model size "<<model_size<<"\n";
        //std::cout<<"Data size "<<data_size<<"\n";
        total_size = model_size + data_size;

    }
    else if(index_type == "bigint"){
        model_size = big_int_alex_index.model_size();
        data_size = big_int_alex_index.data_size();
        //std::cout<<"Model size "<<model_size<<"\n";
        //std::cout<<"Data size "<<data_size<<"\n";
        total_size = model_size + data_size;
    }
    else{
        model_size = unsigned_big_int_alex_index.model_size();
        data_size = unsigned_big_int_alex_index.data_size();
        //std::cout<<"Model size "<<model_size<<"\n";
        //std::cout<<"Data size "<<data_size<<"\n";
        total_size = model_size + data_size;
    }
    //return static_cast<LogicalType::BIGINT>(total_size);
    double model_size_in_mb = static_cast<double>(model_size) / (1024 * 1024);
    double data_size_in_mb = static_cast<double>(data_size) / (1024 * 1024);
    std::cout<<"Model size "<<model_size_in_mb<<" MB\n";
    std::cout<<"Data size "<<data_size_in_mb<<" MB\n";
    double total_size_in_mb = static_cast<double>(total_size) / (1024 * 1024);
    std::cout<<"Size of the Indexing structure "<<total_size_in_mb<<" MB\n";
}

void functionAuxStorage(ClientContext &context, const FunctionParameters &parameters){
    std::string index_type = parameters.values[0].GetValue<string>();
    long long total_size = 0;
    for(const auto& inner_vector:results){
        total_size+=inner_vector.size()*sizeof(inner_vector[0]);
    }
    double total_size_in_mb = static_cast<double>(total_size) / (1024 * 1024);
    std::cout<<"Auxillary storage size "<<total_size_in_mb<<" MB\n";
}
template <typename K>
void print_stats_pgm(){
    if(typeid(K)==typeid(DOUBLE_KEY_TYPE)){
        std::cout << "Total size in bytes: " << double_dynamic_index.size_in_bytes() << " bytes\n";
        std::cout << "Index size in bytes: " << double_dynamic_index.index_size_in_bytes() << " bytes\n";
        std::cout << "Number of elements: " << double_dynamic_index.size() << "\n";

    }
    else if(typeid(K)==typeid(UNSIGNED_INT64_KEY_TYPE)){
        std::cout << "Total size in bytes: " << unsigned_big_int_dynamic_index.size_in_bytes() << " bytes\n";
        std::cout << "Index size in bytes: " << unsigned_big_int_dynamic_index.index_size_in_bytes() << " bytes\n";
        std::cout << "Number of elements: " << unsigned_big_int_dynamic_index.size() << "\n";
    }
    else if(typeid(K)==typeid(INT_KEY_TYPE)){
        std::cout << "Total size in bytes: " << int_dynamic_index.size_in_bytes() << " bytes\n";
        std::cout << "Index size in bytes: " << int_dynamic_index.index_size_in_bytes() << " bytes\n";
        std::cout << "Number of elements: " << int_dynamic_index.size() << "\n";
    }
    else{ //big int
        std::cout << "Total size in bytes: " << big_int_dynamic_index.size_in_bytes() << " bytes\n";
        std::cout << "Big Int Dynamic Index size in bytes: " << big_int_dynamic_index.index_size_in_bytes() << " bytes\n";
        std::cout << "Number of elements: " << big_int_dynamic_index.size() << "\n";
    }

}
template <typename K,typename P>
void bulkLoadIntoIndexPGM(duckdb::Connection & con,std::string table_name,int column_index){
    std::cout<<"General Function with no consequence.\n"; 
}

template<>
void bulkLoadIntoIndexPGM<double,INDEX_PAYLOAD_TYPE>(duckdb::Connection & con,std::string table_name,int column_index){
/*
    Phase 1: Load the data from the table.
    */
    string query = "SELECT * FROM "+table_name+";";
    unique_ptr<MaterializedQueryResult> result = con.Query(query);
    results = result->getContents();
    int num_keys = results.size();
    //std::cout<<"Num Keys : "<<num_keys<<"\n";

   /*
    Phase 2: Bulk load the data from the results vector into the pair array that goes into the index.
   */
   std::vector<std::pair<double,INDEX_PAYLOAD_TYPE>> bulk_load_values;
   bulk_load_values.reserve(num_keys);

 
    int max_key = INT_MIN;
    for (int i=0;i<results.size();i++){
        int row_id = i;
        //std::cout<<"before key"<<"\n";
        auto *data = dynamic_cast<Base *>(results[i][column_index].get());
        auto *data1 = dynamic_cast<Base *>(results[i][column_index + 1].get());
        
        double key_ = static_cast<double_t>(static_cast<DoubleData *>(data)->value);
        double value_ = static_cast<double_t>(static_cast<DoubleData *>(data1)->value);
        
        //std::cout<<"after key"<<"\n";
        bulk_load_values.emplace_back(key_,value_);
    }
    /**
     Phase 3: Sort the bulk load values array based on the key values.
    */

    auto start_time = std::chrono::high_resolution_clock::now();
    std::sort(bulk_load_values.begin(),bulk_load_values.end(),[](auto const& a, auto const& b) { return a.first < b.first; });

    /*
    Phase 4: Bulk load the sorted values into the index.
    */


     for (const auto &pair : bulk_load_values) {
        double_dynamic_index.insert_or_assign(pair.first, pair.second);
    }
    
    auto end_time = std::chrono::high_resolution_clock::now();
    std::chrono::duration<double> elapsed_seconds = end_time - start_time;
    std::cout << "Time taken to bulk load: " << elapsed_seconds.count() << " seconds\n\n\n";
    print_stats_pgm<DOUBLE_KEY_TYPE>();
}

template<>
void bulkLoadIntoIndexPGM<int64_t,INDEX_PAYLOAD_TYPE>(duckdb::Connection & con,std::string table_name,int column_index){
/*
    Phase 1: Load the data from the table.
    */
    string query = "SELECT * FROM "+table_name+";";
    unique_ptr<MaterializedQueryResult> result = con.Query(query);
    results = result->getContents();
    int num_keys = results.size();
    std::cout<<"Num Keys : "<<num_keys<<"\n";

   /*
    Phase 2: Bulk load the data from the results vector into the pair array that goes into the index.
   */
//    std::pair<int64_t,INDEX_PAYLOAD_TYPE>* bulk_load_values = new std::pair<int64_t,INDEX_PAYLOAD_TYPE>[num_keys];

   std::vector<std::pair<double,INDEX_PAYLOAD_TYPE>> bulk_load_values;
   bulk_load_values.reserve(num_keys);

    std::cout<<"Col index "<<column_index<<"\n";    
    int max_key = INT_MIN;
    for (int i=0;i<results.size();i++){
        int row_id = i;
        std::cout<<"before key"<<"\n";
        auto *data = dynamic_cast<Base *>(results[i][column_index].get());
        auto *data1 = dynamic_cast<Base *>(results[i][column_index + 1].get());
        
        int64_t key_ = static_cast<int64_t>(static_cast<BigIntData *>(data)->value);
        double value_ = static_cast<double_t>(static_cast<DoubleData *>(data1)->value);
        
        // std::cout<<"after key: "<<key_<<" Value: "<<value_<<"\n";

        bulk_load_values.emplace_back(key_,value_);

        std::cout<<"Bulk Load values : "<<bulk_load_values[i].first<<" "<<bulk_load_values[i].second<<"\n";
    }

    std::cout<<"Bulk Load Values Size Before Sort: "<<bulk_load_values.size()<<"\n";

    /**
     Phase 3: Sort the bulk load values array based on the key values.
    */
   //Measure time 
    
    auto start_time = std::chrono::high_resolution_clock::now();
    std::sort(bulk_load_values.begin(),bulk_load_values.end(),[](auto const& a, auto const& b) { return a.first < b.first; });
    
    /*
    Phase 4: Bulk load the sorted values into the index.
    */

    std::cout<<"Bulk Load Values Size : "<<bulk_load_values.size()<<"\n";
    
    // big_int_dynamic_index.bulk_load(bulk_load_values, num_keys);

    for (const auto &pair : bulk_load_values) {
        std::cout<<"Key : "<<pair.first<<" Value : "<<pair.second<<"\n";
        big_int_dynamic_index.insert_or_assign(pair.first, pair.second);
        std::cout<<"Index Size : "<<big_int_dynamic_index.size()<<"\n";
    }

    auto end_time = std::chrono::high_resolution_clock::now();
    std::chrono::duration<double> elapsed_seconds = end_time - start_time;
    std::cout << "Time taken to bulk load: " << elapsed_seconds.count() <<" seconds\n\n\n";
    print_stats_pgm<INT64_KEY_TYPE>();
}

template<>
void bulkLoadIntoIndexPGM<UNSIGNED_INT64_KEY_TYPE,INDEX_PAYLOAD_TYPE>(duckdb::Connection & con,std::string table_name,int column_index){
/*
    Phase 1: Load the data from the table.
    */
    string query = "SELECT * FROM "+table_name+";";
    unique_ptr<MaterializedQueryResult> result = con.Query(query);
    results = result->getContents();
    int num_keys = results.size();
    //std::cout<<"Num Keys : "<<num_keys<<"\n";

   /*
    Phase 2: Bulk load the data from the results vector into the pair array that goes into the index.
   */
//    std::pair<UNSIGNED_INT64_KEY_TYPE,INDEX_PAYLOAD_TYPE>* bulk_load_values = new std::pair<UNSIGNED_INT64_KEY_TYPE,INDEX_PAYLOAD_TYPE>[num_keys];
    //std::cout<<"Col index "<<column_index<<"\n";    
    std::vector<std::pair<double,INDEX_PAYLOAD_TYPE>> bulk_load_values;
    bulk_load_values.reserve(num_keys);

    int max_key = INT_MIN;
    for (int i=0;i<results.size();i++){
        int row_id = i;
        //std::cout<<"before key"<<"\n";
        auto *data = dynamic_cast<Base *>(results[i][column_index].get());
        auto *data1 = dynamic_cast<Base *>(results[i][column_index + 1].get());
        
        UNSIGNED_INT64_KEY_TYPE key_ = static_cast<u_int64_t>(static_cast<UBigIntData *>(data)->value);
        double value_ = static_cast<double_t>(static_cast<DoubleData *>(data1)->value);
        
        //std::cout<<"after key"<<"\n";
        bulk_load_values.emplace_back(key_,value_);
    }
    /**
     Phase 3: Sort the bulk load values array based on the key values.
    */
   //Measure time 
    
    auto start_time = std::chrono::high_resolution_clock::now();
    std::sort(bulk_load_values.begin(),bulk_load_values.end(),[](auto const& a, auto const& b) { return a.first < b.first; });
    
    /*
    Phase 4: Bulk load the sorted values into the index.
    */
    
    // unsigned_big_int_dynamic_index.bulk_load(bulk_load_values, num_keys);

    for (const auto &pair : bulk_load_values) {
        unsigned_big_int_dynamic_index.insert_or_assign(pair.first, pair.second);
    }


    auto end_time = std::chrono::high_resolution_clock::now();
    std::chrono::duration<double> elapsed_seconds = end_time - start_time;
    std::cout << "Time taken to bulk load: " << elapsed_seconds.count() <<" seconds\n\n\n";
    print_stats_pgm<UNSIGNED_INT64_KEY_TYPE>();
}

template<>
void bulkLoadIntoIndexPGM<INT_KEY_TYPE,INDEX_PAYLOAD_TYPE>(duckdb::Connection & con,std::string table_name,int column_index){
/*
    Phase 1: Load the data from the table.
    */
    string query = "SELECT * FROM "+table_name+";";
    unique_ptr<MaterializedQueryResult> result = con.Query(query);
    results = result->getContents();
    int num_keys = results.size();
    std::cout<<"Num Keys : "<<num_keys<<"\n";

   /*
    Phase 2: Bulk load the data from the results vector into the pair array that goes into the index.
   */
//    std::pair<INT_KEY_TYPE,INDEX_PAYLOAD_TYPE>* bulk_load_values = new std::pair<INT_KEY_TYPE,INDEX_PAYLOAD_TYPE>[num_keys];
    std::cout<<"Col index "<<column_index<<"\n";    
    std::vector<std::pair<INT_KEY_TYPE,INDEX_PAYLOAD_TYPE>> bulk_load_values;
    bulk_load_values.reserve(num_keys);


    int max_key = INT_MIN;
    for (int i=0;i<results.size();i++){
        int row_id = i;
        if(results[i][column_index]) {
        std::cout<<"before key"<<"\n";
        auto *data = dynamic_cast<Base *>(results[i][column_index].get());
        auto *data1 = dynamic_cast<Base *>(results[i][column_index + 1].get());
        //auto rrr = results[i][column_index].get();
        
        INT_KEY_TYPE key_ = static_cast<int32_t>(static_cast<IntData *>(data)->value);
        double value_ = static_cast<double_t>(static_cast<DoubleData *>(data1)->value);

        std::cout<<"after key"<<"\n";
        bulk_load_values.emplace_back(key_,  static_cast<INDEX_PAYLOAD_TYPE>(i));
        std::cout<<"Key : "<<key_<<" Value : "<<value_<<"\n";
        }
    }
    /**
     Phase 3: Sort the bulk load values array based on the key values.
    */
   //Measure time 
    
    auto start_time = std::chrono::high_resolution_clock::now();
    std::sort(bulk_load_values.begin(),bulk_load_values.end(),[](auto const& a, auto const& b) { return a.first < b.first; });
    
    /*
    Phase 4: Bulk load the sorted values into the index.
    */
    
    // index.bulk_load(bulk_load_values, num_keys);

     for (const auto &pair : bulk_load_values) {
        std::cout<<"Key : "<<pair.first<<" Value : "<<pair.second<<"\n";
        int_dynamic_index.insert_or_assign(pair.first, pair.second);
    }



    auto end_time = std::chrono::high_resolution_clock::now();
    std::chrono::duration<double> elapsed_seconds = end_time - start_time;
    std::cout << "Time taken to bulk load: " << elapsed_seconds.count() <<" seconds\n\n\n";
    print_stats_pgm<INT_KEY_TYPE>();
}

void createPGMIndexPragmaFunction(ClientContext &context, const FunctionParameters &parameters){
    string table_name = parameters.values[0].GetValue<string>();
    string column_name = parameters.values[1].GetValue<string>();

    QualifiedName qname = GetQualifiedName(context, table_name);
    // CheckIfTableExists(context, qname);
    auto &table = Catalog::GetEntry<TableCatalogEntry>(context, qname.catalog, qname.schema, qname.name);
    auto &columnList = table.GetColumns(); 

    vector<string>columnNames = columnList.GetColumnNames();
    vector<LogicalType>columnTypes = columnList.GetColumnTypes();
    int count = 0;
    int column_index = -1;
    LogicalType column_type;
    int col_i = 0;
    for(col_i=0;col_i<columnNames.size();col_i++){
        string curr_col_name = columnNames[col_i];
        LogicalType curr_col_type = columnTypes[col_i];
        if(curr_col_name == column_name){
            column_index = count;
            column_type = curr_col_type;
        }
        count++;
    }

    if(column_index == -1){
        std::cout<<"Column not found "<<"\n";
    }
    else{
        duckdb::Connection con(*context.db);
        std::cout<<"Column found at index "<<column_index<<"\n";
        std::cout<<"Creating an pgm index for this column"<<"\n";
        // std::cout<<"Column Type "<<typeid(column_type).name()<<"\n";
        // std::cout<<"Column Type "<<typeid(double).name()<<"\n";
        std::cout<<"Column type to string "<<column_type.ToString()<<"\n";
        std::string columnTypeName = column_type.ToString();
        if(columnTypeName == "DOUBLE"){
            bulkLoadIntoIndexPGM<DOUBLE_KEY_TYPE,INDEX_PAYLOAD_TYPE>(con,table_name,column_index);
            index_type_table_name_map.insert({"double",{table_name,column_name}});
        }
        else if(columnTypeName == "BIGINT"){
            bulkLoadIntoIndexPGM<INT64_KEY_TYPE,INDEX_PAYLOAD_TYPE>(con,table_name,column_index);
            index_type_table_name_map.insert({"bigint",{table_name,column_name}});
        }
        else if(columnTypeName == "UBIGINT"){
            bulkLoadIntoIndexPGM<UNSIGNED_INT64_KEY_TYPE,INDEX_PAYLOAD_TYPE>(con,table_name,column_index);
            index_type_table_name_map.insert({"ubigint",{table_name,column_name}});
        }
        else if(columnTypeName == "INTEGER"){
            bulkLoadIntoIndexPGM<INT_KEY_TYPE,INDEX_PAYLOAD_TYPE>(con,table_name,column_index);
            index_type_table_name_map.insert({"int",{table_name,column_name}});
        }
        else{
            std::cout<<"Unsupported column type for alex indexing (for now) "<<"\n";
        }
        //bulkLoadIntoIndex<typeid(column_type).name(),INDEX_PAYLOAD_TYPE>(con,table_name,column_index);
    }
}

inline void RadixScalarFun(DataChunk &args, ExpressionState &state, Vector &result) {
    auto &name_vector = args.data[0];
    UnaryExecutor::Execute<string_t, string_t>(
	    name_vector, result, args.size(),
	    [&](string_t name) {
			return StringVector::AddString(result, "Radix "+name.GetString()+" 🐥");;
        });
}

inline void RadixOpenSSLVersionScalarFun(DataChunk &args, ExpressionState &state, Vector &result) {
    auto &name_vector = args.data[0];
    UnaryExecutor::Execute<string_t, string_t>(
	    name_vector, result, args.size(),
	    [&](string_t name) {
			return StringVector::AddString(result, "Radix " + name.GetString() +
                                                     ", my linked OpenSSL version is " +
                                                     OPENSSL_VERSION_TEXT );;
        });
}

/*
Bulk Load into Index functions
*/
template <typename T>
void BulkLoadRadixSpline(duckdb::Connection &con, const std::string &table_name, int column_index, const std::string &map_key) {
    // Ensure T is one of the allowed types
    static_assert(std::is_same<T, uint32_t>::value || std::is_same<T, uint64_t>::value,
                  "BulkLoadRadixSpline only supports uint32_t and uint64_t.");

    // Query the table
    std::string query = "SELECT * FROM " + table_name + ";";
    unique_ptr<MaterializedQueryResult> res = con.Query(query);

    if (!res || res->HasError()) {
        throw std::runtime_error("Failed to fetch data from table: " + table_name);
    }

    // Fetch results
    auto result = res->getContents();

    // Pre-allocate keys
    std::vector<T> keys;
    keys.reserve(result.size());

    for (size_t i = 0; i < result.size(); i++) {
        if (result[i][column_index]) {
            auto *data = dynamic_cast<Base *>(result[i][column_index].get());
            int id1 = static_cast<int>(static_cast<UIntData *>(data)->id);
            int id2 = static_cast<int>(static_cast<UBigIntData *>(data)->id);

            if (id1 == 30 || id2 == 30) {
                keys.push_back(static_cast<uint32_t>(static_cast<UIntData *>(data)->value));
            } else if (id1 == 31 || id2 == 31) {
                keys.push_back(static_cast<uint64_t>(static_cast<UBigIntData *>(data)->value));
            }
        }
    }

    // Ensure keys are sorted
    std::sort(keys.begin(), keys.end());

    // Build RadixSpline
    rs::Builder<T> builder(keys.front(), keys.back(), kNumRadixBits, kMaxError);
    int num = 0;
    for (const auto &key : keys) {
        builder.AddKey(key);
        num++;
    }

    auto finalized_spline = builder.Finalize();

    // Collect statistics
    RadixSplineStats stats;
    stats.num_keys = keys.size();
    if (!keys.empty()) {
        stats.min_key = keys.front();
        stats.max_key = keys.back();
        stats.average_gap = (keys.size() > 1) ? static_cast<double>(keys.back() - keys.front()) / (keys.size() - 1) : 0.0;
    }

    // Store in the appropriate map
    if constexpr (std::is_same<T, uint32_t>::value) {
        radix_spline_map_int32[map_key] = std::move(finalized_spline);
        radix_spline_stats_map_int32[map_key] = stats;
    } else if constexpr (std::is_same<T, uint64_t>::value) {
        radix_spline_map_int64[map_key] = std::move(finalized_spline);
        radix_spline_stats_map_int64[map_key] = stats;
    }

    std::cout << "RadixSpline successfully created for " << map_key << " Total Keys Added : "<< num << ".\n";
}

/**
 * PragmaFunction to Load the data
*/
void createRadixSplineIndexPragmaFunction(ClientContext &context, const FunctionParameters &parameters) {
    string table_name = parameters.values[0].GetValue<string>();
    string column_name = parameters.values[1].GetValue<string>();

    // Get the qualified name for the table
    QualifiedName qname = GetQualifiedName(context, table_name);

    // Get the table entry
    auto &table = Catalog::GetEntry<TableCatalogEntry>(context, qname.catalog, qname.schema, qname.name);
    auto &columnList = table.GetColumns();

    // Find the column index and type
    vector<string> columnNames = columnList.GetColumnNames();
    vector<LogicalType> columnTypes = columnList.GetColumnTypes();
    int column_index = -1;
    LogicalType column_type;

    for (size_t i = 0; i < columnNames.size(); i++) {
        if (columnNames[i] == column_name) {
            column_index = i;
            column_type = columnTypes[i];
            break;
        }
    }

    // Check if column was found
    if (column_index == -1) {
        std::cout << "Column '" << column_name << "' not found in table '" << table_name << "'.\n";
        return;
    }

    duckdb::Connection con(*context.db);

    // Determine column type and build RadixSpline
    string columnTypeName = column_type.ToString();
    if (columnTypeName == "UBIGINT") {
        BulkLoadRadixSpline<uint64_t>(con, qname.name, column_index, qname.catalog + "." + qname.schema + "." + qname.name + "." + column_name);
    } else if (columnTypeName == "UINTEGER") {
        BulkLoadRadixSpline<uint32_t>(con, qname.name, column_index, qname.catalog + "." + qname.schema + "." + qname.name + "." + column_name);
    } else {
        std::cout << "Unsupported column type '" << columnTypeName << "' for RadixSpline indexing.\n";
    }
}

/**
 * Function to lookup a value using the RadixSpline index
*/
void RadixSplineLookupPragmaFunction(ClientContext &context, const FunctionParameters &parameters) {
    string table_name = parameters.values[0].GetValue<string>();
    string column_name = parameters.values[1].GetValue<string>();
    string lookup_key_str = parameters.values[2].GetValue<string>();

    // Parse the lookup key
    uint64_t lookup_key = std::stoull(lookup_key_str);

    // Create the map key
    QualifiedName qname = GetQualifiedName(context, table_name);
    string map_key = qname.catalog + "." + qname.schema + "." + qname.name + "." + column_name;

    // Determine which RadixSpline map to use
    if (radix_spline_map_int64.find(map_key) != radix_spline_map_int64.end()) {
        // Lookup in the uint64_t RadixSpline map
        const auto &radix_spline = radix_spline_map_int64[map_key];
        size_t estimated_position = radix_spline.GetEstimatedPosition(lookup_key);
        std::cout << "Estimated position for key " << lookup_key << " is: " << estimated_position << std::endl;
    } else if (radix_spline_map_int32.find(map_key) != radix_spline_map_int32.end()) {
        // Lookup in the uint32_t RadixSpline map
        uint32_t lookup_key_32 = static_cast<uint32_t>(lookup_key);
        const auto &radix_spline = radix_spline_map_int32[map_key];
        size_t estimated_position = radix_spline.GetEstimatedPosition(lookup_key_32);
        std::cout << "Estimated position for key " << lookup_key_32 << " is: " << estimated_position << std::endl;
    } else {
        std::cout << "RadixSpline index not found for " << map_key << ". Please ensure you have created the index first." << std::endl;
    }
}

/**
 * Function to delete a RadixSpline index
*/
void DeleteRadixSplineIndexPragmaFunction(ClientContext &context, const FunctionParameters &parameters) {
    string table_name = parameters.values[0].GetValue<string>();
    string column_name = parameters.values[1].GetValue<string>();

    // Create the map key
    QualifiedName qname = GetQualifiedName(context, table_name);
    string map_key = qname.catalog + "." + qname.schema + "." + qname.name + "." + column_name;

    // Determine which RadixSpline map to delete from
    if (radix_spline_map_int64.find(map_key) != radix_spline_map_int64.end()) {
        radix_spline_map_int64.erase(map_key);
        std::cout << "RadixSpline index deleted for " << map_key << ".\n";
    } else if (radix_spline_map_int32.find(map_key) != radix_spline_map_int32.end()) {
        radix_spline_map_int32.erase(map_key);
        std::cout << "RadixSpline index deleted for " << map_key << ".\n";
    } else {
        std::cout << "RadixSpline index not found for " << map_key << ".\n";
    }
}

/**
 * Function to lookup range values using the RadixSpline index
*/
void RadixSplineRangeLookupPragmaFunction(ClientContext &context, const FunctionParameters &parameters) {
    string table_name = parameters.values[0].GetValue<string>();
    string column_name = parameters.values[1].GetValue<string>();
    string start_key_str = parameters.values[2].GetValue<string>();
    string end_key_str = parameters.values[3].GetValue<string>();

    // Parse the range keys
    uint64_t start_key = std::stoull(start_key_str);
    uint64_t end_key = std::stoull(end_key_str);

    // Create the map key
    QualifiedName qname = GetQualifiedName(context, table_name);
    string map_key = qname.catalog + "." + qname.schema + "." + qname.name + "." + column_name;

    // Perform range lookup in the appropriate RadixSpline map
    if (radix_spline_map_int64.find(map_key) != radix_spline_map_int64.end()) {
        const auto &radix_spline = radix_spline_map_int64[map_key];
        size_t start_position = radix_spline.GetEstimatedPosition(start_key);
        size_t end_position = radix_spline.GetEstimatedPosition(end_key);
        std::cout << "Estimated positions for range (" << start_key << " - " << end_key << ") are: "
                  << "start: " << start_position << ", end: " << end_position << std::endl;
    } else if (radix_spline_map_int32.find(map_key) != radix_spline_map_int32.end()) {
        uint32_t start_key_32 = static_cast<uint32_t>(start_key);
        uint32_t end_key_32 = static_cast<uint32_t>(end_key);
        const auto &radix_spline = radix_spline_map_int32[map_key];
        size_t start_position = radix_spline.GetEstimatedPosition(start_key_32);
        size_t end_position = radix_spline.GetEstimatedPosition(end_key_32);
        std::cout << "Estimated positions for range (" << start_key_32 << " - " << end_key_32 << ") are: "
                  << "start: " << start_position << ", end: " << end_position << std::endl;
    } else {
        std::cout << "RadixSpline index not found for " << map_key << ". Please ensure you have created the index first." << std::endl;
    }
}

/**
 * Function for collecting the stats.
*/
void RadixSplineStatsPragmaFunction(ClientContext &context, const FunctionParameters &parameters) {
    string table_name = parameters.values[0].GetValue<string>();
    string column_name = parameters.values[1].GetValue<string>();

    // Create the map key
    QualifiedName qname = GetQualifiedName(context, table_name);
    string map_key = qname.catalog + "." + qname.schema + "." + qname.name + "." + column_name;

    // Determine which RadixSpline stats map to use
    if (radix_spline_stats_map_int64.find(map_key) != radix_spline_stats_map_int64.end()) {
        const auto &stats = radix_spline_stats_map_int64[map_key];
        std::cout << "Statistics for RadixSpline index '" << map_key << "':\n";
        std::cout << " - Number of keys: " << stats.num_keys << "\n";
        std::cout << " - Minimum key: " << stats.min_key << "\n";
        std::cout << " - Maximum key: " << stats.max_key << "\n";
        std::cout << " - Average gap between keys: " << stats.average_gap << "\n";
    } else if (radix_spline_stats_map_int32.find(map_key) != radix_spline_stats_map_int32.end()) {
        const auto &stats = radix_spline_stats_map_int32[map_key];
        std::cout << "Statistics for RadixSpline index '" << map_key << "':\n";
        std::cout << " - Number of keys: " << stats.num_keys << "\n";
        std::cout << " - Minimum key: " << stats.min_key << "\n";
        std::cout << " - Maximum key: " << stats.max_key << "\n";
        std::cout << " - Average gap between keys: " << stats.average_gap << "\n";
    } else {
        std::cout << "RadixSpline index not found for " << map_key << ". Please ensure you have created the index first.\n";
    }
}

/**
 * Load Benchmark Data into Table with Batching
 * 
 * Template function to load key-value pairs into DuckDB in batches.
 * 
 * @param keys - The keys to insert into the table.
 * @param con - Connection to DuckDB.
 * @param tableName - Name of the table to insert the data into.
 * @param NUM_KEYS - Total number of keys.
 * @param num_batches_insert - Number of batches.
 * @param per_batch - Number of records per batch.
 */
template <typename K, typename P>
void load_benchmark_data_into_table(const std::vector<K> &keys, duckdb::Connection &con, std::string tableName, int NUM_KEYS, int num_batches_insert, int per_batch) {
    // This function will load key-value pairs into the table in batches
    int starting = 0;
    int ending = 0;

    std::string query_prefix = "INSERT INTO " + tableName + " VALUES ";

    for (int i = 0; i < num_batches_insert; i++) {
        std::cout << "Inserting batch " << i << "\n";
        
        starting = i * per_batch;
        ending = std::min(starting + per_batch, NUM_KEYS);
        std::ostringstream tuple_stream;

        // Generate batch insertion tuples
        std::mt19937_64 gen_payload(std::random_device{}());

        for (int vti = starting; vti < ending; vti++) {
            K key = keys[vti];
            P random_payload = static_cast<P>(gen_payload());

            tuple_stream << "(" << key << ", " << random_payload << ")";
            if (vti != ending - 1) {
                tuple_stream << ",";
            }
        }

        std::string to_execute_query = query_prefix + tuple_stream.str() + ";";
        auto res = con.Query(to_execute_query);

        if (!res->HasError()) {
            std::cout << "Batch " << i << " inserted successfully.\n";
        } else {
            std::cerr << "Error inserting batch " << i << ": " << res->GetError() << "\n";
        }
    }

    std::cout << "Data successfully inserted into table " << tableName << ".\n";
}

/**
 * LoadBenchmarkFromFile - Loads data from a fixed binary file path into DuckDB and inserts in batches.
 *
 * @param context - The ClientContext of the current DuckDB connection.
 */
void LoadBenchmarkFromFile(ClientContext &context, const FunctionParameters &parameters) {
    // Fixed file path from which the data will be loaded
    std::string file_path = "/Users/jishnusm/Desktop/classes/AdvancedDataStores/Project/Project2/radix/test/ycsb-200M.bin";

    // Open the file for reading
    std::ifstream infile(file_path, std::ios::binary);
    if (!infile.is_open()) {
        throw std::runtime_error("Failed to open file: " + file_path);
    }

    // Vector to hold the keys read from the binary file
    std::vector<uint64_t> keys;

    // Read the file content into the vector
    uint64_t value;
    while (infile.read(reinterpret_cast<char *>(&value), sizeof(uint64_t))) {
        keys.push_back(value);
    }

    infile.close();

    if (keys.empty()) {
        throw std::runtime_error("The file is empty or there was an issue reading the file: " + file_path);
    }

    // Step to create a table in DuckDB and insert benchmark data
    // Define the table name
    std::string table_name = "benchmark_table_ycsb_200M";

    // Create a connection using the ClientContext
    duckdb::Connection con(*context.db);

    // SQL query to create the table with two columns: 'key' and 'payload'
    std::string create_query = "CREATE TABLE " + table_name + " (key UBIGINT, payload DOUBLE);";
    con.Query(create_query);

    // Load data into the table using batch insertion
    int num_batches_insert = 1000; // Number of batches to insert
    int per_batch = 1000; // Number of records per batch
    int NUM_KEYS = keys.size();

    load_benchmark_data_into_table<uint64_t, double>(keys, con, table_name, NUM_KEYS, num_batches_insert, per_batch);
}

/**
 * Function to check the insertion and compare with DuckDB
*/
void functionSearchBenchmarkRadixSpline(ClientContext &context, const FunctionParameters &parameters) {
    std::string table_name = parameters.values[0].GetValue<string>();
    std::string column_name = parameters.values[1].GetValue<string>();
    int num_keys_to_search = parameters.values[2].GetValue<int>();
    std::vector<int> batch_sizes = {100, 1000, 5000, 10000, 100000}; // Predefined batch sizes

    duckdb::Connection con(*context.db);

    // Generate random keys to search
    std::vector<uint64_t> keys(num_keys_to_search);
    std::random_device rd;
    std::mt19937_64 gen(rd()); // 64-bit random number generator
    std::uniform_int_distribution<uint64_t> dist(1, UINT64_MAX); // Full uint64_t range
    for (int i = 0; i < num_keys_to_search; ++i) {
        keys[i] = dist(gen);
    }

    // Iterate over batch sizes
    for (int batch_size : batch_sizes) {
        std::cout << "Benchmarking batch size: " << batch_size << "\n";

        // Search keys in DuckDB
        auto start_duckdb_search_time = std::chrono::high_resolution_clock::now();
        for (int i = 0; i < num_keys_to_search; i += batch_size) {
            for (int j = i; j < std::min(i + batch_size, num_keys_to_search); ++j) {
                std::string search_query =
                    "SELECT " + column_name + " FROM " + table_name + " WHERE " + column_name + " = " + std::to_string(keys[j]) + ";";
                auto res = con.Query(search_query);
                if (res->HasError()) {
                    std::cerr << "Error querying DuckDB: " << res->GetError() << "\n";
                }
            }
        }
        auto end_duckdb_search_time = std::chrono::high_resolution_clock::now();
        std::chrono::duration<double> duckdb_search_duration = end_duckdb_search_time - start_duckdb_search_time;
        std::cout << "DuckDB search time for batch size " << batch_size << ": " << duckdb_search_duration.count() << " seconds\n";

        // Search keys using RadixSpline index
        auto start_radix_search_time = std::chrono::high_resolution_clock::now();
        for (int i = 0; i < num_keys_to_search; i += batch_size) {
            for (int j = i; j < std::min(i + batch_size, num_keys_to_search); ++j) {
                std::string pragma_search_index = "PRAGMA lookup_radixspline_index('" + table_name + "', '" + column_name + "', '" +
                                                  std::to_string(keys[j]) + "');";
                auto res = con.Query(pragma_search_index);
                if (res->HasError()) {
                    std::cerr << "Error querying RadixSpline index: " << res->GetError() << "\n";
                }
            }
        }
        auto end_radix_search_time = std::chrono::high_resolution_clock::now();
        std::chrono::duration<double> radix_search_duration = end_radix_search_time - start_radix_search_time;
        std::cout << "RadixSpline search time for batch size " << batch_size << ": " << radix_search_duration.count() << " seconds\n";

        // Compare performance
        std::cout << "Performance comparison for batch size " << batch_size << ":\n";
        std::cout << " - DuckDB search time: " << duckdb_search_duration.count() << " seconds\n";
        std::cout << " - RadixSpline search time: " << radix_search_duration.count() << " seconds\n";
        std::cout << "\n";
    }
}

/**
 * Load Functions: 
 * 
*/
static void LoadInternal(DatabaseInstance &instance) {
    // Register a scalar function
    auto alex_scalar_function = ScalarFunction("alex", {LogicalType::VARCHAR}, LogicalType::VARCHAR, AlexScalarFun);
    ExtensionUtil::RegisterFunction(instance, alex_scalar_function);

    // Register another scalar function
    auto alex_openssl_version_scalar_function = ScalarFunction("alex_openssl_version", {LogicalType::VARCHAR},
                                                LogicalType::VARCHAR, AlexOpenSSLVersionScalarFun);
    ExtensionUtil::RegisterFunction(instance, alex_openssl_version_scalar_function);

    auto create_alex_index_function = PragmaFunction::PragmaCall("create_alex_index", createAlexIndexPragmaFunction, {LogicalType::VARCHAR, LogicalType::VARCHAR},{});
    ExtensionUtil::RegisterFunction(instance, create_alex_index_function);
    auto create_pgm_index_function = PragmaFunction::PragmaCall("create_pgm_index", createPGMIndexPragmaFunction, {LogicalType::VARCHAR, LogicalType::VARCHAR},{});
    ExtensionUtil::RegisterFunction(instance, create_pgm_index_function);
    
    // The arguments for the load benchmark data function are the table name, benchmark name and the number of elements to bulk load.
    auto loadBenchmarkData = PragmaFunction::PragmaCall("load_benchmark",functionLoadBenchmark,{LogicalType::VARCHAR,LogicalType::VARCHAR,LogicalType::INTEGER,LogicalType::INTEGER},{});
    ExtensionUtil::RegisterFunction(instance,loadBenchmarkData);

    auto runBenchmarkWorkload = PragmaFunction::PragmaCall("run_lookup_benchmark",functionRunLookupBenchmark,{LogicalType::VARCHAR,LogicalType::VARCHAR},{});
    ExtensionUtil::RegisterFunction(instance,runBenchmarkWorkload);

    auto create_art_index_function = PragmaFunction::PragmaCall("create_art_index",functionCreateARTIndex,{LogicalType::VARCHAR,LogicalType::VARCHAR},LogicalType::INVALID); 
    ExtensionUtil::RegisterFunction(instance,create_art_index_function);
    
    auto insert_into_table_function = PragmaFunction::PragmaCall("insert_into_table",functionInsertIntoTable,{LogicalType::VARCHAR,LogicalType::VARCHAR,LogicalType::VARCHAR,LogicalType::VARCHAR},{});
    ExtensionUtil::RegisterFunction(instance,insert_into_table_function);

    //Benchmark name,index.
    auto runBenchmarkOneBatch = PragmaFunction::PragmaCall("run_benchmark_one_batch",functionRunBenchmarkOneBatch,{LogicalType::VARCHAR,LogicalType::VARCHAR,LogicalType::VARCHAR},{});
    ExtensionUtil::RegisterFunction(instance,runBenchmarkOneBatch);

    auto searchUsingAlexIndex = PragmaFunction::PragmaCall("alex_find",functionAlexFind,{LogicalType::VARCHAR,LogicalType::VARCHAR},{});
    ExtensionUtil::RegisterFunction(instance,searchUsingAlexIndex);

    auto findSize = PragmaFunction::PragmaCall("alex_size",functionAlexSize,{LogicalType::VARCHAR},{});
    ExtensionUtil::RegisterFunction(instance,findSize);

    auto auxillaryStorageSizes = PragmaFunction::PragmaCall("auxillary_storage_size",functionAuxStorage,{LogicalType::VARCHAR},{});
    ExtensionUtil::RegisterFunction(instance,auxillaryStorageSizes);

    auto runInsertionBenchmark = PragmaFunction::PragmaCall("run_insertion_benchmark",functionRunInsertionBenchmark,{LogicalType::VARCHAR,LogicalType::VARCHAR,LogicalType::VARCHAR, LogicalType::INTEGER},{});
    ExtensionUtil::RegisterFunction(instance,runInsertionBenchmark);
   
    // Register the create_radixspline_index pragma
    auto create_radixspline_index_function = PragmaFunction::PragmaCall(
        "create_radixspline_index",                             // Name of the pragma
        createRadixSplineIndexPragmaFunction,                  // Function to call
        {LogicalType::VARCHAR, LogicalType::VARCHAR},          // Expected argument types (table name, column name)
        {}
    );
    ExtensionUtil::RegisterFunction(instance, create_radixspline_index_function);

    // Register the lookup_radixspline pragma
    auto lookup_radixspline_function = PragmaFunction::PragmaCall(
        "lookup_radixspline_index",                          // Name of the pragma
        RadixSplineLookupPragmaFunction,                     // Function to call
        {LogicalType::VARCHAR, LogicalType::VARCHAR, LogicalType::VARCHAR}, // Expected argument types (table name, column name, lookup key)
        {}
    );
    ExtensionUtil::RegisterFunction(instance, lookup_radixspline_function);

    // Delete a RadixSpline Index
    auto delete_radixspline_function = PragmaFunction::PragmaCall(
        "delete_radixspline_index",                               // Name of the pragma
        DeleteRadixSplineIndexPragmaFunction,                    // Function to call
        {LogicalType::VARCHAR, LogicalType::VARCHAR}, // Expected argument types (table name, column name)
        {}
    );
    ExtensionUtil::RegisterFunction(instance, delete_radixspline_function);

    // For range lookups
    auto range_lookup_radixspline_function = PragmaFunction::PragmaCall(
        "range_lookup_radixspline",                             // Name of the pragma
        RadixSplineRangeLookupPragmaFunction,                   // Function to call
        {LogicalType::VARCHAR, LogicalType::VARCHAR, LogicalType::VARCHAR, LogicalType::VARCHAR}, // Expected argument types (table name, column name, starting range, ending range)
        {}
    );
    ExtensionUtil::RegisterFunction(instance, range_lookup_radixspline_function);

    // Register the stats_radixspline pragma
    auto stats_radixspline_function = PragmaFunction::PragmaCall(
        "stats_radixspline",                             // Name of the pragma
        RadixSplineStatsPragmaFunction,                  // Function to call
        {LogicalType::VARCHAR, LogicalType::VARCHAR},    // Expected argument types (table name, column name)
        {}
    );
    ExtensionUtil::RegisterFunction(instance, stats_radixspline_function);

    // Register the LoadBenchmarkFromFile function as a pragma
    auto load_benchmark_function = PragmaFunction::PragmaCall(
        "load_benchmark_from_file",   // Name of the pragma
        LoadBenchmarkFromFile,        // Function to call
        {}                            // No arguments required
    );
    ExtensionUtil::RegisterFunction(instance, load_benchmark_function);

    // Register the search_benchmark_radixspline function as a pragma
    auto search_benchmark_radixspline = PragmaFunction::PragmaCall(
        "search_benchmark_radixspline", 
        functionSearchBenchmarkRadixSpline,
        {LogicalType::VARCHAR, LogicalType::VARCHAR, LogicalType::INTEGER}, 
        {}
    );
    ExtensionUtil::RegisterFunction(instance, search_benchmark_radixspline);
}

void AlexExtension::Load(DuckDB &db) {
	LoadInternal(*db.instance);
}
std::string AlexExtension::Name() {
	return "alex";
}

}// namespace duckdb

extern "C" {

DUCKDB_EXTENSION_API void alex_init(duckdb::DatabaseInstance &db) {
    duckdb::DuckDB db_wrapper(db);
    db_wrapper.LoadExtension<duckdb::AlexExtension>(); // this function pretty much loads the extension onto the db call.
}

DUCKDB_EXTENSION_API const char *alex_version() {
	return duckdb::DuckDB::LibraryVersion();
}
}

#ifndef DUCKDB_EXTENSION_MAIN
#error DUCKDB_EXTENSION_MAIN not defined
#endif
