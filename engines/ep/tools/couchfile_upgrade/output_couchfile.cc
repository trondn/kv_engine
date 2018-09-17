/* -*- Mode: C++; tab-width: 4; c-basic-offset: 4; indent-tabs-mode: nil -*- */
/*
 *     Copyright 2018 Couchbase, Inc
 *
 *   Licensed under the Apache License, Version 2.0 (the "License");
 *   you may not use this file except in compliance with the License.
 *   You may obtain a copy of the License at
 *
 *       http://www.apache.org/licenses/LICENSE-2.0
 *
 *   Unless required by applicable law or agreed to in writing, software
 *   distributed under the License is distributed on an "AS IS" BASIS,
 *   WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *   See the License for the specific language governing permissions and
 *   limitations under the License.
 */

#include "output_couchfile.h"
#include "input_couchfile.h"

#include <mcbp/protocol/unsigned_leb128.h>
#include <nlohmann/json.hpp>

#include <iostream>

namespace Collections {

OutputCouchFile::OutputCouchFile(OptionsSet options,
                                 const std::string& filename,
                                 CollectionID newCollection,
                                 size_t maxBufferedSize)
    : CouchFile(options, filename, COUCHSTORE_OPEN_FLAG_CREATE),
      collection(newCollection),
      bufferedOutput(maxBufferedSize) {
}

void OutputCouchFile::commit() {
    if (!bufferedOutput.empty()) {
        verbose("commit is triggering a write");
        writeDocuments();
    }

    verbose("commit");

    auto errcode = couchstore_commit(db);
    if (errcode) {
        throw std::runtime_error(
                "OutputCouchFile::commit couchstore_commit failed errcode:" +
                std::to_string(errcode));
    }
}

// Moving a document to a collection in the context of this upgrade is to
// prefix the key with a unsigned_leb128 collection-id (cid)
std::string OutputCouchFile::moveDocToCollection(const sized_buf in,
                                                 CollectionID cid) const {
    cb::mcbp::unsigned_leb128<CollectionIDType> encodedCollection(cid);
    std::string rv(in.size + encodedCollection.size(), ' ');
    auto next =
            std::copy_n(reinterpret_cast<const char*>(encodedCollection.data()),
                        encodedCollection.size(),
                        rv.begin());
    std::copy(in.buf, in.buf + in.size, next);
    return rv;
}

void OutputCouchFile::processDocument(const Doc* doc, const DocInfo* docinfo) {
    if (bufferedOutput.addDocument(
                moveDocToCollection(doc->id, collection), doc, docinfo)) {
        verbose("processDocument triggering write");
        writeDocuments();
    }
}

void OutputCouchFile::writeDocuments() {
    verbose("writeDocuments size:" + std::to_string(bufferedOutput.size()));
    bufferedOutput.prepareForWrite();

    auto errcode = couchstore_save_documents(
            db,
            bufferedOutput.getDocs(),
            bufferedOutput.getDocInfos(),
            unsigned(bufferedOutput.size()),
            COMPRESS_DOC_BODIES | COUCHSTORE_SEQUENCE_AS_IS);

    if (errcode) {
        throw std::runtime_error(
                "OutputCouchFile::writeDocuments couchstore_save_documents "
                "errcode:" +
                std::to_string(errcode));
    }

    bufferedOutput.reset();
}

void OutputCouchFile::setVBState(const std::string& inputVBS) {
    writeLocalDocument("_local/vbstate", inputVBS);
}

void OutputCouchFile::writeLocalDocument(const std::string& documentName,
                                         const std::string& value) const {
    LocalDoc localDoc;
    localDoc.id.buf = const_cast<char*>(documentName.c_str());
    localDoc.id.size = documentName.size();
    localDoc.json.buf = const_cast<char*>(value.c_str());
    localDoc.json.size = value.size();
    localDoc.deleted = 0;

    auto errcode = couchstore_save_local_document(db, &localDoc);
    if (errcode) {
        throw std::runtime_error(
                "OutputCouchFile::writeLocalDocument failed "
                "couchstore_open_local_document documentName:" +
                documentName + " value:" + value + " errcode:" +
                std::to_string(errcode));
    }
    verbose("writeLocalDocument(" + documentName + ", " + value + ") success");
}

void OutputCouchFile::writeUpgradeBegin(const InputCouchFile& input) const {
    writeSupportsCollections(input.getLocalDocument("_local/vbstate"), false);
}

void OutputCouchFile::writeUpgradeComplete(const InputCouchFile& input) const {
    writeSupportsCollections(input.getLocalDocument("_local/vbstate"), true);
}

void OutputCouchFile::writeSupportsCollections(const std::string& vbs,
                                               bool value) const {
    nlohmann::json json;
    try {
        json = nlohmann::json::parse(vbs);
    } catch (const nlohmann::json::exception& e) {
        throw std::invalid_argument(
                "OutputCouchFile::writePartiallyNamespaced cannot parse "
                " json:" +
                vbs + " exception:" + e.what());
    }
    json[CollectionsSupportedKey] = value;
    writeLocalDocument("_local/vbstate", json.dump());
}

OutputCouchFile::BufferedOutputDocuments::Document::Document(
        const std::string& newDocKey, const Doc* doc, const DocInfo* docInfo)
    : newDocKey(newDocKey), newDoc(*doc), newDocInfo(*docInfo), doc(doc) {
    // Update the ID fields with the newDocKey
    newDoc.id.buf = const_cast<char*>(this->newDocKey.data());
    newDoc.id.size = this->newDocKey.size();
    newDocInfo.id = newDoc.id;

    // Copy the rev_meta
    if (docInfo->rev_meta.buf) {
        std::copy(docInfo->rev_meta.buf,
                  docInfo->rev_meta.buf + docInfo->rev_meta.size,
                  std::back_inserter(revMeta));
        newDocInfo.rev_meta = {revMeta.data(), revMeta.size()};
    }
}

OutputCouchFile::BufferedOutputDocuments::Document::Document(Document&& other) {
    newDocKey = std::move(other.newDocKey);
    revMeta = std::move(other.revMeta);
    newDoc = other.newDoc;
    newDocInfo = other.newDocInfo;

    // Now make sure pointers make sense
    newDoc.id.buf = const_cast<char*>(newDocKey.data());
    newDoc.id.size = newDocKey.size();
    newDocInfo.id = newDoc.id;

    if (other.newDocInfo.rev_meta.buf) {
        // point to this revMeta
        newDocInfo.rev_meta = {revMeta.data(), revMeta.size()};
    }

    doc = other.doc;
    other.doc = nullptr;
}

OutputCouchFile::BufferedOutputDocuments::Document::~Document() {
    if (doc) {
        couchstore_free_document(const_cast<Doc*>(doc));
    }
}

OutputCouchFile::BufferedOutputDocuments::BufferedOutputDocuments(
        size_t maxBufferedSize)
    : maxBufferedSize(maxBufferedSize) {
}

bool OutputCouchFile::BufferedOutputDocuments::addDocument(
        const std::string& newDocKey, const Doc* doc, const DocInfo* docInfo) {
    outputDocuments.emplace_back(newDocKey, doc, docInfo);

    approxBufferedSize +=
            (newDocKey.size() + docInfo->rev_meta.size + doc->id.size);

    auto vectorSizes = (sizeof(Document) * outputDocuments.size()) +
                       (sizeof(Doc*) * outputDocs.size()) +
                       (sizeof(DocInfo*) * outputDocInfos.size());

    // Return if we should now trigger the write of the buffer
    return (approxBufferedSize + vectorSizes) >= maxBufferedSize;
}

void OutputCouchFile::BufferedOutputDocuments::prepareForWrite() {
    for (auto& doc : outputDocuments) {
        outputDocs.push_back(&doc.newDoc);
        outputDocInfos.push_back(&doc.newDocInfo);
    }
}

} // end namespace Collections