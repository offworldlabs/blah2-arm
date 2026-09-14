#include "Map.h"
#include "data/meta/Constants.h"
#include <iostream>
#include <cstdlib>
#include <chrono>
#include <cstdint>

#include "rapidjson/document.h"
#include "rapidjson/writer.h"
#include "rapidjson/stringbuffer.h"
#include "rapidjson/filewritestream.h"

// constructor
template <class T>
Map<T>::Map(uint32_t _nRows, uint32_t _nCols)
{
  nRows = _nRows;
  nCols = _nCols;
  std::vector<std::vector<T>> tmp(nRows, std::vector<T>(nCols, {1}));
  data = tmp;
}

template <class T>
void Map<T>::set_row(uint32_t i, std::vector<T> row)
{
  //data[i].swap(row);
  for (uint32_t j = 0; j < nCols; j++)
  {
    data[i][j] = row[j];
  }
}

template <class T>
void Map<T>::set_col(uint32_t i, std::vector<T> col)
{
  for (uint32_t j = 0; j < nRows; j++)
  {
    data[j][i] = col[j];
  }
}

template <class T>
uint32_t Map<T>::get_nRows()
{
  return nRows;
}

template <class T>
uint32_t Map<T>::get_nCols()
{
  return nCols;
}

template <class T>
std::vector<T> Map<T>::get_row(uint32_t row)
{
  return data[row];
}

template <class T>
std::vector<T> Map<T>::get_col(uint32_t col)
{
  std::vector<T> colData;

  for (uint32_t i = 0; i < nRows; i++)
  {
    colData.push_back(data[i][col]);
  }

  return colData;
}

template <class T>
Map<double> *Map<T>::get_map_db()
{
  Map<double> *map = new Map<double>(nRows, nCols);

  for (uint32_t i = 0; i < nRows; i++)
  {
    for (uint32_t j = 0; j < nCols; j++)
    {
      map->data[i][j] = (double)10 * std::log10(std::abs(data[i][j]));
    }
  }

  return map;
}

template <class T>
void Map<T>::print()
{
  for (uint32_t i = 0; i < nRows; i++)
  {
    for (uint32_t j = 0; j < nCols; j++)
    {
      std::cout << data[i][j];
      std::cout << " ";
    }
    std::cout << std::endl;
  }
}

template <class T>
uint32_t Map<T>::doppler_hz_to_bin(double dopplerHz)
{
  for (size_t i = 0; i < doppler.size(); i++)
  {
    if (dopplerHz == doppler[i])
    {
      return (int) i;
    }
  }
  return 0;
}

template <class T>
std::string Map<T>::to_json(uint64_t timestamp)
{
  rapidjson::Document document;
  document.SetObject();
  rapidjson::Document::AllocatorType &allocator = document.GetAllocator();

  // store data array
  rapidjson::Value array(rapidjson::kArrayType);
  for (size_t i = 0; i < data.size(); i++)
  {
    rapidjson::Value subarray(rapidjson::kArrayType);
    for (size_t j = 0; j < data[i].size(); j++)
    {
      subarray.PushBack(10 * std::log10(std::abs(data[i][j])) - noisePower, document.GetAllocator());
    }
    array.PushBack(subarray, document.GetAllocator());
  }

  // store delay array
  rapidjson::Value arrayDelay(rapidjson::kArrayType);
  for (size_t i = 0; i < delay.size(); i++)
  {
    arrayDelay.PushBack(delay[i], allocator);
  }

  // store Doppler array
  rapidjson::Value arrayDoppler(rapidjson::kArrayType);
  for (uint32_t i = 0; i < get_nRows(); i++)
  {
    arrayDoppler.PushBack(doppler[i], allocator);
  }

  document.AddMember("timestamp", timestamp, allocator);
  document.AddMember("nRows", nRows, allocator);
  document.AddMember("nCols", nCols, allocator);
  document.AddMember("noisePower", noisePower, allocator);
  document.AddMember("maxPower", maxPower, allocator);
  document.AddMember("delay", arrayDelay, allocator);
  document.AddMember("doppler", arrayDoppler, allocator);
  document.AddMember(rapidjson::Value("data", document.GetAllocator()).Move(), array, document.GetAllocator());

  rapidjson::StringBuffer strbuf;
  rapidjson::Writer<rapidjson::StringBuffer> writer(strbuf);
  writer.SetMaxDecimalPlaces(2);
  document.Accept(writer);

  return strbuf.GetString();
}

template <class T>
std::string Map<T>::to_json_km(uint64_t timestamp, uint32_t fs)
{
  // One pass, no DOM. The pair to_json() + delay_bin_to_km() serialised the
  // whole map, parsed all of it back, rewrote the delay axis and serialised
  // again, so a 301 x 411 map went through the writer twice and the parser
  // once to convert 411 delay values. Key order and SetMaxDecimalPlaces match
  // the old pair exactly, so the bytes are identical.
  rapidjson::StringBuffer strbuf;
  rapidjson::Writer<rapidjson::StringBuffer> writer(strbuf);
  writer.SetMaxDecimalPlaces(2);

  writer.StartObject();
  writer.Key("timestamp");
  writer.Uint64(timestamp);
  writer.Key("nRows");
  writer.Uint(nRows);
  writer.Key("nCols");
  writer.Uint(nCols);
  writer.Key("noisePower");
  writer.Double(noisePower);
  writer.Key("maxPower");
  writer.Double(maxPower);

  writer.Key("delay");
  writer.StartArray();
  for (size_t i = 0; i < delay.size(); i++)
  {
    writer.Double(1.0 * delay[i] * (Constants::c / (double)fs) / 1000);
  }
  writer.EndArray();

  writer.Key("doppler");
  writer.StartArray();
  for (uint32_t i = 0; i < get_nRows(); i++)
  {
    writer.Double(doppler[i]);
  }
  writer.EndArray();

  writer.Key("data");
  writer.StartArray();
  for (size_t i = 0; i < data.size(); i++)
  {
    writer.StartArray();
    for (size_t j = 0; j < data[i].size(); j++)
    {
      writer.Double(10 * std::log10(std::abs(data[i][j])) - noisePower);
    }
    writer.EndArray();
  }
  writer.EndArray();
  writer.EndObject();

  return strbuf.GetString();
}

template <class T>
std::string Map<T>::delay_bin_to_km(std::string json, uint32_t fs)
{
  rapidjson::Document document;
  document.SetObject();
  rapidjson::Document::AllocatorType &allocator = document.GetAllocator();
  document.Parse(json.c_str());

  document["delay"].Clear();
  for (size_t i = 0; i < delay.size(); i++)
  {
    document["delay"].PushBack(1.0*delay[i]*(Constants::c/(double)fs)/1000, allocator);
  }

  rapidjson::StringBuffer strbuf;
  rapidjson::Writer<rapidjson::StringBuffer> writer(strbuf);
  writer.SetMaxDecimalPlaces(2);
  document.Accept(writer);

  return strbuf.GetString();
}

template <class T>
void Map<T>::set_metrics()
{
  // get map noise level
  double value;
  double noisePower = 0;
  double maxPower = 0;
  for (uint32_t i = 0; i < nRows; i++)
  {
    for (uint32_t j = 0; j < nCols; j++)
    {
      value = 10 * std::log10(std::abs(data[i][j]));
      noisePower = noisePower + value;
      maxPower = (maxPower < value) ? value : maxPower;
    }
  }
  noisePower = noisePower / (nRows * nCols);
  this->noisePower = noisePower;
  this->maxPower = maxPower - noisePower;
}

// Bound on the save.map file.
//
// A node writes this to its own SD card and nothing ever truncates it: save()
// appends one range-doppler frame per CPI to a single file for as long as blah2
// runs. Measured on a node: 2.21 MB a frame against an observed CPI of 1.17 s,
// so 1.87 MB/s, or 158 GB/day. That fills a 24 GB data partition in under four
// hours.
//
// save.map is off by default and retina-gui does not offer it, but user.yml is
// an unrestricted deep merge that support edits over SSH, so "not exposed" is
// not the same as "cannot happen" and the bound belongs with the writer.
//
// Past the bound the array is left closed and valid and further maps are
// dropped. Rotating or truncating instead would discard the beginning of the
// very capture the flag was turned on to collect.
static constexpr uintmax_t SAVE_MAP_MAX_BYTES = 512ULL * 1024 * 1024;
static bool saveBoundReached = false;

template <class T>
bool Map<T>::save(std::string _json, std::string filename)
{
  using namespace rapidjson;

  rapidjson::Document document;

  // create file if it doesn't exist
  if (FILE *fp = fopen(filename.c_str(), "r"); !fp)
  {
    if (fp = fopen(filename.c_str(), "w"); !fp)
      return false;
    fputs("[]", fp);
    fclose(fp);
  }

  // add the document to the file
  if (FILE *fp = fopen(filename.c_str(), "rb+"); fp)
  {
    // stop before the bound rather than after it
    std::fseek(fp, 0, SEEK_END);
    if (long size = std::ftell(fp);
        size < 0 || static_cast<uintmax_t>(size) + _json.length() + 2 > SAVE_MAP_MAX_BYTES)
    {
      std::fclose(fp);
      if (!saveBoundReached)
      {
        saveBoundReached = true;
        std::cerr << "Warning: " << filename << " reached the "
                  << (SAVE_MAP_MAX_BYTES / (1024 * 1024))
                  << " MB bound, no further maps will be saved" << std::endl;
      }
      return false;
    }

    // check if first is [
    std::fseek(fp, 0, SEEK_SET);
    if (getc(fp) != '[')
    {
      std::fclose(fp);
      return false;
    }

    // is array empty?
    bool isEmpty = false;
    if (getc(fp) == ']')
      isEmpty = true;

    // check if last is ]
    std::fseek(fp, -1, SEEK_END);
    if (getc(fp) != ']')
    {
      std::fclose(fp);
      return false;
    }

    // replace ] by ,
    fseek(fp, -1, SEEK_END);
    if (!isEmpty)
      fputc(',', fp);

    // add json element
    fwrite(_json.c_str(), sizeof(char), _json.length(), fp);

    // close the array
    std::fputc(']', fp);
    fclose(fp);
    return true;
  }
  return false;
}

// allowed types
template class Map<std::complex<double>>;
template class Map<double>;
