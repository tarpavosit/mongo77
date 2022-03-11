/**
 *    Copyright (C) 2022-present MongoDB, Inc.
 *
 *    This program is free software: you can redistribute it and/or modify
 *    it under the terms of the Server Side Public License, version 1,
 *    as published by MongoDB, Inc.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    Server Side Public License for more details.
 *
 *    You should have received a copy of the Server Side Public License
 *    along with this program. If not, see
 *    <http://www.mongodb.com/licensing/server-side-public-license>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the Server Side Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#include "mongo/platform/basic.h"

#include <algorithm>
#include <array>
#include <string>
#include <third_party/murmurhash3/MurmurHash3.h>
#include <unordered_map>
#include <vector>

#include "mongo/base/data_range.h"
#include "mongo/base/string_data.h"
#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/bsontypes.h"
#include "mongo/bson/json.h"
#include "mongo/crypto/encryption_fields_gen.h"
#include "mongo/crypto/fle_crypto.h"
#include "mongo/crypto/fle_field_schema_gen.h"
#include "mongo/db/catalog/collection_options.h"
#include "mongo/db/fle_crud.h"
#include "mongo/db/matcher/schema/encrypt_schema_gen.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/ops/write_ops_gen.h"
#include "mongo/db/ops/write_ops_parsers.h"
#include "mongo/db/repl/replication_coordinator_mock.h"
#include "mongo/db/repl/storage_interface.h"
#include "mongo/db/repl/storage_interface_impl.h"
#include "mongo/db/service_context.h"
#include "mongo/db/service_context_d_test_fixture.h"
#include "mongo/executor/network_interface_mock.h"
#include "mongo/idl/idl_parser.h"
#include "mongo/platform/random.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/uuid.h"

namespace mongo {
namespace {

class FLEQueryTestImpl : public FLEQueryInterface {
public:
    FLEQueryTestImpl(OperationContext* opCtx, repl::StorageInterface* storage)
        : _opCtx(opCtx), _storage(storage) {}
    ~FLEQueryTestImpl() = default;

    BSONObj getById(const NamespaceString& nss, BSONElement element) final;

    BSONObj getById(const NamespaceString& nss, PrfBlock block) {
        auto doc = BSON("v" << BSONBinData(block.data(), block.size(), BinDataGeneral));
        BSONElement element = doc.firstElement();
        return getById(nss, element);
    }

    uint64_t countDocuments(const NamespaceString& nss) final;

    void insertDocument(const NamespaceString& nss, BSONObj obj, bool translateDuplicateKey) final;

    BSONObj deleteWithPreimage(const NamespaceString& nss,
                               const EncryptionInformation& ei,
                               const write_ops::DeleteCommandRequest& deleteRequest) final;

    BSONObj updateWithPreimage(const NamespaceString& nss,
                               const EncryptionInformation& ei,
                               const write_ops::UpdateCommandRequest& updateRequest) final;

private:
    OperationContext* _opCtx;
    repl::StorageInterface* _storage;
};

BSONObj FLEQueryTestImpl::getById(const NamespaceString& nss, BSONElement element) {
    auto obj = BSON("_id" << element);
    auto swDoc = _storage->findById(_opCtx, nss, obj.firstElement());
    if (swDoc.getStatus() == ErrorCodes::NoSuchKey) {
        return BSONObj();
    }

    return uassertStatusOK(swDoc);
}

uint64_t FLEQueryTestImpl::countDocuments(const NamespaceString& nss) {
    return uassertStatusOK(_storage->getCollectionCount(_opCtx, nss));
}

void FLEQueryTestImpl::insertDocument(const NamespaceString& nss,
                                      BSONObj obj,
                                      bool translateDuplicateKey) {
    repl::TimestampedBSONObj tb;
    tb.obj = obj;

    auto status = _storage->insertDocument(_opCtx, nss, tb, 0);

    uassertStatusOK(status);
}

BSONObj FLEQueryTestImpl::deleteWithPreimage(const NamespaceString& nss,
                                             const EncryptionInformation& ei,
                                             const write_ops::DeleteCommandRequest& deleteRequest) {
    // A limit of the API, we can delete by _id and get the pre-image so we limit our unittests to
    // this
    ASSERT_EQ(deleteRequest.getDeletes().size(), 1);
    auto deleteOpEntry = deleteRequest.getDeletes()[0];
    ASSERT_EQ("_id"_sd, deleteOpEntry.getQ().firstElementFieldNameStringData());

    auto swDoc = _storage->deleteById(_opCtx, nss, deleteOpEntry.getQ().firstElement());

    // Some of the unit tests delete documents that do not exist
    if (swDoc.getStatus() == ErrorCodes::NoSuchKey) {
        return BSONObj();
    }

    return uassertStatusOK(swDoc);
}

BSONObj FLEQueryTestImpl::updateWithPreimage(const NamespaceString& nss,
                                             const EncryptionInformation& ei,
                                             const write_ops::UpdateCommandRequest& updateRequest) {
    // A limit of the API, we can delete by _id and get the pre-image so we limit our unittests to
    // this
    ASSERT_EQ(updateRequest.getUpdates().size(), 1);
    auto updateOpEntry = updateRequest.getUpdates()[0];
    ASSERT_EQ("_id"_sd, updateOpEntry.getQ().firstElementFieldNameStringData());

    BSONObj preimage = getById(nss, updateOpEntry.getQ().firstElement());

    uassertStatusOK(_storage->upsertById(_opCtx,
                                         nss,
                                         updateOpEntry.getQ().firstElement(),
                                         updateOpEntry.getU().getUpdateModifier()));

    return preimage;
}

std::array<uint8_t, 96> indexVec = {
    0x44, 0xba, 0xd4, 0x1d, 0x6a, 0x9b, 0xdd, 0x38, 0x60, 0xc8, 0xfa, 0x9d, 0xf1, 0x1b, 0x8a, 0x75,
    0x30, 0x61, 0x91, 0xb4, 0xd0, 0x17, 0x2e, 0xa7, 0x15, 0x18, 0xf1, 0x36, 0xc4, 0xef, 0x71, 0x68,
    0x7e, 0xad, 0x69, 0xb7, 0x64, 0xcf, 0x37, 0x9a, 0xaa, 0x82, 0x22, 0xf7, 0x3a, 0xf5, 0xfa, 0x7a,
    0x6b, 0xf2, 0xbf, 0x99, 0x52, 0xa5, 0xcf, 0x51, 0xee, 0xdf, 0xa6, 0x06, 0xb5, 0x0f, 0xa3, 0x49,
    0x4d, 0x41, 0x7f, 0x53, 0xfd, 0xa2, 0x63, 0x5d, 0xa2, 0xcd, 0x3d, 0x78, 0x18, 0x32, 0x1e, 0x35,
    0x1c, 0x74, 0xca, 0x19, 0x92, 0x3a, 0x1d, 0xc6, 0x2a, 0x7f, 0x72, 0x52, 0x0b, 0xce, 0x59, 0x6d};

std::array<uint8_t, 96> userVec = {
    0x7c, 0xc9, 0x46, 0xd8, 0x6b, 0x19, 0x3b, 0x75, 0xfb, 0xcf, 0x0d, 0xd1, 0xf1, 0xd3, 0xb1, 0x3a,
    0x61, 0x99, 0xaa, 0xb3, 0x1c, 0x7e, 0x6a, 0xe1, 0xe3, 0x8a, 0xd0, 0x4b, 0xd6, 0xa3, 0xcb, 0xaa,
    0x13, 0x86, 0x15, 0xfc, 0xcf, 0x45, 0xe7, 0xd1, 0x4a, 0x69, 0x44, 0xff, 0x01, 0x85, 0xb1, 0x88,
    0x2a, 0xa3, 0x96, 0xbb, 0xd4, 0x92, 0x0c, 0x02, 0x0f, 0xe7, 0x22, 0xf6, 0xf7, 0x68, 0x49, 0x93,
    0x1c, 0xff, 0x62, 0x4f, 0x8e, 0xdd, 0x4c, 0x70, 0x53, 0x78, 0x0e, 0xf9, 0x20, 0x0f, 0xba, 0xa1,
    0xe7, 0x82, 0x84, 0x36, 0x2e, 0x28, 0x0e, 0xca, 0xfd, 0x16, 0x65, 0xbd, 0xa3, 0x7e, 0xa4, 0xb0};

constexpr auto kIndexKeyId = "12345678-1234-9876-1234-123456789012"_sd;
constexpr auto kUserKeyId = "ABCDEFAB-1234-9876-1234-123456789012"_sd;
static UUID indexKeyId = uassertStatusOK(UUID::parse(kIndexKeyId.toString()));
static UUID userKeyId = uassertStatusOK(UUID::parse(kUserKeyId.toString()));

std::vector<char> testValue = {0x10, 0x11, 0x12, 0x13, 0x14, 0x15, 0x16, 0x17, 0x18, 0x19};
std::vector<char> testValue2 = {0x20, 0x21, 0x22, 0x23, 0x24, 0x25, 0x26, 0x27, 0x28, 0x29};

class TestKeyVault : public FLEKeyVault {
public:
    TestKeyVault() : _random(123456) {}

    FLEIndexKey indexKey{KeyMaterial(indexVec.begin(), indexVec.end())};

    FLEUserKey userKey{KeyMaterial(userVec.begin(), userVec.end())};

    KeyMaterial getKey(const UUID& uuid) override;

    uint64_t getCount() const {
        return _dynamicKeys.size();
    }

private:
    PseudoRandom _random;
    stdx::unordered_map<UUID, KeyMaterial, UUID::Hash> _dynamicKeys;
};

KeyMaterial TestKeyVault::getKey(const UUID& uuid) {
    if (uuid == indexKeyId) {
        return indexKey.data;
    } else if (uuid == userKeyId) {
        return userKey.data;
    } else {
        if (_dynamicKeys.find(uuid) != _dynamicKeys.end()) {
            return _dynamicKeys[uuid];
        }

        std::vector<uint8_t> materialVector(96);
        _random.fill(&materialVector[0], materialVector.size());
        KeyMaterial material(materialVector.begin(), materialVector.end());
        _dynamicKeys.insert({uuid, material});
        return material;
    }
}

UUID fieldNameToUUID(StringData field) {
    std::array<uint8_t, UUID::kNumBytes> buf;

    MurmurHash3_x86_128(field.rawData(), field.size(), 123456, buf.data());

    return UUID::fromCDR(buf);
}

std::string fieldNameFromInt(uint64_t i) {
    return "field" + std::to_string(i);
}

class FleCrudTest : public ServiceContextMongoDTest {
private:
    void setUp() final;
    void tearDown() final;

protected:
    void createCollection(const NamespaceString& ns);

    void assertDocumentCounts(uint64_t edc, uint64_t esc, uint64_t ecc, uint64_t ecoc);

    void doSingleInsert(int id, BSONElement element);
    void doSingleInsert(int id, BSONObj obj);

    void doSingleDelete(int id);

    void doSingleUpdate(int id, BSONElement element);
    void doSingleUpdate(int id, BSONObj obj);
    void doSingleUpdateWithUpdateDoc(int id, BSONObj update);

    using ValueGenerator = std::function<std::string(StringData fieldName, uint64_t row)>;

    void doSingleWideInsert(int id, uint64_t fieldCount, ValueGenerator func);

    void validateDocument(int id, boost::optional<BSONObj> doc);

    ESCTwiceDerivedTagToken getTestESCToken(BSONElement value);
    ESCTwiceDerivedTagToken getTestESCToken(BSONObj obj);
    ESCTwiceDerivedTagToken getTestESCToken(StringData name, StringData value);

    ECCDerivedFromDataTokenAndContentionFactorToken getTestECCToken(BSONElement value);

    ECCDocument getECCDocument(ECCDerivedFromDataTokenAndContentionFactorToken token, int position);

    std::vector<char> generatePlaceholder(UUID keyId, BSONElement value);

protected:
    /**
     * Looks up the current ReplicationCoordinator.
     * The result is cast to a ReplicationCoordinatorMock to provide access to test features.
     */
    repl::ReplicationCoordinatorMock* _getReplCoord() const;

    ServiceContext::UniqueOperationContext _opCtx;

    repl::StorageInterface* _storage{nullptr};

    std::unique_ptr<FLEQueryTestImpl> _queryImpl;

    TestKeyVault _keyVault;

    NamespaceString _edcNs{"test.edc"};
    NamespaceString _escNs{"test.esc"};
    NamespaceString _eccNs{"test.ecc"};
    NamespaceString _ecocNs{"test.ecoc"};
};

void FleCrudTest::setUp() {
    ServiceContextMongoDTest::setUp();
    auto service = getServiceContext();

    repl::ReplicationCoordinator::set(service,
                                      std::make_unique<repl::ReplicationCoordinatorMock>(service));

    _opCtx = cc().makeOperationContext();

    repl::StorageInterface::set(service, std::make_unique<repl::StorageInterfaceImpl>());
    _storage = repl::StorageInterface::get(service);

    _queryImpl = std::make_unique<FLEQueryTestImpl>(_opCtx.get(), _storage);

    createCollection(_edcNs);
    createCollection(_escNs);
    createCollection(_eccNs);
    createCollection(_ecocNs);
}

void FleCrudTest::tearDown() {
    _opCtx = {};
    ServiceContextMongoDTest::tearDown();
}

void FleCrudTest::createCollection(const NamespaceString& ns) {
    CollectionOptions collectionOptions;
    collectionOptions.uuid = UUID::gen();
    auto statusCC = _storage->createCollection(
        _opCtx.get(), NamespaceString(ns.db(), ns.coll()), collectionOptions);
    ASSERT_OK(statusCC);
}

ConstDataRange toCDR(BSONElement element) {
    return ConstDataRange(element.value(), element.value() + element.valuesize());
}

ESCTwiceDerivedTagToken FleCrudTest::getTestESCToken(BSONElement element) {
    auto c1token = FLELevel1TokenGenerator::generateCollectionsLevel1Token(
        _keyVault.getIndexKeyById(indexKeyId).key);
    auto escToken = FLECollectionTokenGenerator::generateESCToken(c1token);

    auto escDataToken =
        FLEDerivedFromDataTokenGenerator::generateESCDerivedFromDataToken(escToken, toCDR(element));
    auto escContentionToken = FLEDerivedFromDataTokenAndContentionFactorTokenGenerator::
        generateESCDerivedFromDataTokenAndContentionFactorToken(escDataToken, 0);

    return FLETwiceDerivedTokenGenerator::generateESCTwiceDerivedTagToken(escContentionToken);
}

ESCTwiceDerivedTagToken FleCrudTest::getTestESCToken(BSONObj obj) {
    return getTestESCToken(obj.firstElement());
}

ESCTwiceDerivedTagToken FleCrudTest::getTestESCToken(StringData name, StringData value) {

    auto doc = BSON("i" << value);
    auto element = doc.firstElement();

    UUID keyId = fieldNameToUUID(name);

    auto c1token = FLELevel1TokenGenerator::generateCollectionsLevel1Token(
        _keyVault.getIndexKeyById(keyId).key);
    auto escToken = FLECollectionTokenGenerator::generateESCToken(c1token);

    auto escDataToken =
        FLEDerivedFromDataTokenGenerator::generateESCDerivedFromDataToken(escToken, toCDR(element));
    auto escContentionToken = FLEDerivedFromDataTokenAndContentionFactorTokenGenerator::
        generateESCDerivedFromDataTokenAndContentionFactorToken(escDataToken, 0);

    return FLETwiceDerivedTokenGenerator::generateESCTwiceDerivedTagToken(escContentionToken);
}

ECCDerivedFromDataTokenAndContentionFactorToken FleCrudTest::getTestECCToken(BSONElement element) {
    auto c1token = FLELevel1TokenGenerator::generateCollectionsLevel1Token(
        _keyVault.getIndexKeyById(indexKeyId).key);
    auto eccToken = FLECollectionTokenGenerator::generateECCToken(c1token);

    auto eccDataToken =
        FLEDerivedFromDataTokenGenerator::generateECCDerivedFromDataToken(eccToken, toCDR(element));
    return FLEDerivedFromDataTokenAndContentionFactorTokenGenerator::
        generateECCDerivedFromDataTokenAndContentionFactorToken(eccDataToken, 0);
}

ECCDocument FleCrudTest::getECCDocument(ECCDerivedFromDataTokenAndContentionFactorToken token,
                                        int position) {

    auto tag = FLETwiceDerivedTokenGenerator::generateECCTwiceDerivedTagToken(token);
    auto value = FLETwiceDerivedTokenGenerator::generateECCTwiceDerivedValueToken(token);

    BSONObj doc = _queryImpl->getById(_eccNs, ECCCollection::generateId(tag, position));
    ASSERT_FALSE(doc.isEmpty());

    return uassertStatusOK(ECCCollection::decryptDocument(value, doc));
}


std::vector<char> FleCrudTest::generatePlaceholder(UUID keyId, BSONElement value) {
    FLE2EncryptionPlaceholder ep;

    ep.setAlgorithm(mongo::Fle2AlgorithmInt::kEquality);
    ep.setUserKeyId(keyId);
    ep.setIndexKeyId(keyId);
    ep.setValue(value);
    ep.setType(mongo::Fle2PlaceholderType::kInsert);
    ep.setMaxContentionCounter(0);

    BSONObj obj = ep.toBSON();

    std::vector<char> v;
    v.resize(obj.objsize() + 1);
    v[0] = static_cast<uint8_t>(EncryptedBinDataType::kFLE2Placeholder);
    std::copy(obj.objdata(), obj.objdata() + obj.objsize(), v.begin() + 1);
    return v;
}

EncryptedFieldConfig getTestEncryptedFieldConfig() {

    constexpr auto schema = R"({
    "escCollection": "esc",
    "eccCollection": "ecc",
    "ecocCollection": "ecoc",
    "fields": [
        {
            "keyId":
                            {
                                "$uuid": "12345678-1234-9876-1234-123456789012"
                            }
                        ,
            "path": "encrypted",
            "bsonType": "string",
            "queries": {"queryType": "equality"}

        }
    ]
})";

    return EncryptedFieldConfig::parse(IDLParserErrorContext("root"), fromjson(schema));
}

void FleCrudTest::assertDocumentCounts(uint64_t edc, uint64_t esc, uint64_t ecc, uint64_t ecoc) {
    ASSERT_EQ(_queryImpl->countDocuments(_edcNs), edc);
    ASSERT_EQ(_queryImpl->countDocuments(_escNs), esc);
    ASSERT_EQ(_queryImpl->countDocuments(_eccNs), ecc);
    ASSERT_EQ(_queryImpl->countDocuments(_ecocNs), ecoc);
}

// Auto generate key ids from field id
void FleCrudTest::doSingleWideInsert(int id, uint64_t fieldCount, ValueGenerator func) {
    BSONObjBuilder builder;
    builder.append("_id", id);
    builder.append("plainText", "sample");

    for (uint64_t i = 0; i < fieldCount; i++) {
        auto name = fieldNameFromInt(i);
        auto value = func(name, id);
        auto doc = BSON("I" << value);
        UUID uuid = fieldNameToUUID(name);
        auto buf = generatePlaceholder(uuid, doc.firstElement());
        builder.appendBinData(name, buf.size(), BinDataType::Encrypt, buf.data());
    }

    auto clientDoc = builder.obj();

    auto result = FLEClientCrypto::generateInsertOrUpdateFromPlaceholders(clientDoc, &_keyVault);

    auto serverPayload = EDCServerCollection::getEncryptedFieldInfo(result);

    auto efc = getTestEncryptedFieldConfig();

    processInsert(_queryImpl.get(), _edcNs, serverPayload, efc, result);
}


void FleCrudTest::validateDocument(int id, boost::optional<BSONObj> doc) {

    auto doc1 = BSON("_id" << id);
    auto updatedDoc = _queryImpl->getById(_edcNs, doc1.firstElement());

    std::cout << "Updated Doc: " << updatedDoc << std::endl;

    auto efc = getTestEncryptedFieldConfig();
    FLEClientCrypto::validateDocument(updatedDoc, efc, &_keyVault);

    // Decrypt document
    auto decryptedDoc = FLEClientCrypto::decryptDocument(updatedDoc, &_keyVault);

    if (doc.has_value()) {
        // Remove this so the round-trip is clean
        decryptedDoc = decryptedDoc.removeField(kSafeContent);

        ASSERT_BSONOBJ_EQ(doc.value(), decryptedDoc);
    }
}

// Use different keys for index and user
std::vector<char> generateSinglePlaceholder(BSONElement value) {
    FLE2EncryptionPlaceholder ep;

    ep.setAlgorithm(mongo::Fle2AlgorithmInt::kEquality);
    ep.setUserKeyId(userKeyId);
    ep.setIndexKeyId(indexKeyId);
    ep.setValue(value);
    ep.setType(mongo::Fle2PlaceholderType::kInsert);
    ep.setMaxContentionCounter(0);

    BSONObj obj = ep.toBSON();

    std::vector<char> v;
    v.resize(obj.objsize() + 1);
    v[0] = static_cast<uint8_t>(EncryptedBinDataType::kFLE2Placeholder);
    std::copy(obj.objdata(), obj.objdata() + obj.objsize(), v.begin() + 1);
    return v;
}

void FleCrudTest::doSingleInsert(int id, BSONElement element) {
    auto buf = generateSinglePlaceholder(element);
    BSONObjBuilder builder;
    builder.append("_id", id);
    builder.append("counter", 1);
    builder.append("plainText", "sample");
    builder.appendBinData("encrypted", buf.size(), BinDataType::Encrypt, buf.data());

    auto clientDoc = builder.obj();

    auto result = FLEClientCrypto::generateInsertOrUpdateFromPlaceholders(clientDoc, &_keyVault);

    auto serverPayload = EDCServerCollection::getEncryptedFieldInfo(result);

    auto efc = getTestEncryptedFieldConfig();

    processInsert(_queryImpl.get(), _edcNs, serverPayload, efc, result);
}

void FleCrudTest::doSingleInsert(int id, BSONObj obj) {
    doSingleInsert(id, obj.firstElement());
}

void FleCrudTest::doSingleUpdate(int id, BSONObj obj) {
    doSingleUpdate(id, obj.firstElement());
}

void FleCrudTest::doSingleUpdate(int id, BSONElement element) {
    auto buf = generateSinglePlaceholder(element);
    BSONObjBuilder builder;
    builder.append("$inc", BSON("counter" << 1));
    builder.append("$set",
                   BSON("encrypted" << BSONBinData(buf.data(), buf.size(), BinDataType::Encrypt)));
    auto clientDoc = builder.obj();
    auto result = FLEClientCrypto::generateInsertOrUpdateFromPlaceholders(clientDoc, &_keyVault);

    doSingleUpdateWithUpdateDoc(id, result);
}

void FleCrudTest::doSingleUpdateWithUpdateDoc(int id, BSONObj update) {
    auto efc = getTestEncryptedFieldConfig();
    auto doc = EncryptionInformationHelpers::encryptionInformationSerializeForDelete(
        _edcNs, efc, &_keyVault);
    auto ei = EncryptionInformation::parse(IDLParserErrorContext("test"), doc);

    write_ops::UpdateOpEntry entry;
    entry.setQ(BSON("_id" << id));
    entry.setU(
        write_ops::UpdateModification(update, write_ops::UpdateModification::ClassicTag{}, false));

    write_ops::UpdateCommandRequest updateRequest(_edcNs);
    updateRequest.setUpdates({entry});
    updateRequest.getWriteCommandRequestBase().setEncryptionInformation(ei);

    processUpdate(_queryImpl.get(), updateRequest);
}

void FleCrudTest::doSingleDelete(int id) {

    auto efc = getTestEncryptedFieldConfig();

    auto doc = EncryptionInformationHelpers::encryptionInformationSerializeForDelete(
        _edcNs, efc, &_keyVault);

    auto ei = EncryptionInformation::parse(IDLParserErrorContext("test"), doc);

    write_ops::DeleteOpEntry entry;
    entry.setQ(BSON("_id" << id));
    entry.setMulti(false);

    write_ops::DeleteCommandRequest deleteRequest(_edcNs);
    deleteRequest.setDeletes({entry});
    deleteRequest.getWriteCommandRequestBase().setEncryptionInformation(ei);

    processDelete(_queryImpl.get(), deleteRequest);
}

// Insert one document
TEST_F(FleCrudTest, InsertOne) {
    auto doc = BSON("encrypted"
                    << "secret");
    auto element = doc.firstElement();

    doSingleInsert(1, element);

    assertDocumentCounts(1, 1, 0, 1);

    ASSERT_FALSE(_queryImpl->getById(_escNs, ESCCollection::generateId(getTestESCToken(element), 1))
                     .isEmpty());
}

// Insert two documents with same values
TEST_F(FleCrudTest, InsertTwoSame) {

    auto doc = BSON("encrypted"
                    << "secret");
    auto element = doc.firstElement();
    doSingleInsert(1, element);
    doSingleInsert(2, element);

    assertDocumentCounts(2, 2, 0, 2);

    ASSERT_FALSE(_queryImpl->getById(_escNs, ESCCollection::generateId(getTestESCToken(element), 1))
                     .isEmpty());
    ASSERT_FALSE(_queryImpl->getById(_escNs, ESCCollection::generateId(getTestESCToken(element), 2))
                     .isEmpty());
}

// Insert two documents with different values
TEST_F(FleCrudTest, InsertTwoDifferent) {

    doSingleInsert(1,
                   BSON("encrypted"
                        << "secret"));
    doSingleInsert(2,
                   BSON("encrypted"
                        << "topsecret"));

    assertDocumentCounts(2, 2, 0, 2);

    ASSERT_FALSE(_queryImpl
                     ->getById(_escNs,
                               ESCCollection::generateId(getTestESCToken(BSON("encrypted"
                                                                              << "secret")),
                                                         1))
                     .isEmpty());
    ASSERT_FALSE(_queryImpl
                     ->getById(_escNs,
                               ESCCollection::generateId(getTestESCToken(BSON("encrypted"
                                                                              << "topsecret")),
                                                         1))
                     .isEmpty());
}

// Insert 1 document with 100 fields
TEST_F(FleCrudTest, Insert100Fields) {

    uint64_t fieldCount = 100;
    ValueGenerator valueGenerator = [](StringData fieldName, uint64_t row) {
        return fieldName.toString();
    };
    doSingleWideInsert(1, fieldCount, valueGenerator);

    assertDocumentCounts(1, fieldCount, 0, fieldCount);

    for (uint64_t field = 0; field < fieldCount; field++) {
        auto fieldName = fieldNameFromInt(field);

        ASSERT_FALSE(
            _queryImpl
                ->getById(
                    _escNs,
                    ESCCollection::generateId(
                        getTestESCToken(fieldName, valueGenerator(fieldNameFromInt(field), 0)), 1))
                .isEmpty());
    }
}

// Insert 100 documents each with 20 fields with 7 distinct values per field
TEST_F(FleCrudTest, Insert20Fields50Rows) {

    uint64_t fieldCount = 20;
    uint64_t rowCount = 50;

    ValueGenerator valueGenerator = [](StringData fieldName, uint64_t row) {
        return fieldName.toString() + std::to_string(row % 7);
    };


    for (uint64_t row = 0; row < rowCount; row++) {
        doSingleWideInsert(row, fieldCount, valueGenerator);
    }

    assertDocumentCounts(rowCount, rowCount * fieldCount, 0, rowCount * fieldCount);

    for (uint64_t row = 0; row < rowCount; row++) {
        for (uint64_t field = 0; field < fieldCount; field++) {
            auto fieldName = fieldNameFromInt(field);

            int count = (row / 7) + 1;

            ASSERT_FALSE(
                _queryImpl
                    ->getById(_escNs,
                              ESCCollection::generateId(
                                  getTestESCToken(fieldName,
                                                  valueGenerator(fieldNameFromInt(field), row)),
                                  count))
                    .isEmpty());
        }
    }
}

#define ASSERT_ECC_DOC(assertElement, assertPosition, assertStart, assertEnd)            \
    {                                                                                    \
        auto _eccDoc = getECCDocument(getTestECCToken((assertElement)), assertPosition); \
        ASSERT(_eccDoc.valueType == ECCValueType::kNormal);                              \
        ASSERT_EQ(_eccDoc.start, assertStart);                                           \
        ASSERT_EQ(_eccDoc.end, assertEnd);                                               \
    }

// Insert and delete one document
TEST_F(FleCrudTest, InsertAndDeleteOne) {
    auto doc = BSON("encrypted"
                    << "secret");
    auto element = doc.firstElement();

    doSingleInsert(1, element);

    assertDocumentCounts(1, 1, 0, 1);

    ASSERT_FALSE(_queryImpl->getById(_escNs, ESCCollection::generateId(getTestESCToken(element), 1))
                     .isEmpty());

    doSingleDelete(1);

    assertDocumentCounts(0, 1, 1, 2);

    getECCDocument(getTestECCToken(element), 1);
}

// Insert two documents, and delete both
TEST_F(FleCrudTest, InsertTwoSamAndDeleteTwo) {
    auto doc = BSON("encrypted"
                    << "secret");
    auto element = doc.firstElement();

    doSingleInsert(1, element);
    doSingleInsert(2, element);

    assertDocumentCounts(2, 2, 0, 2);

    ASSERT_FALSE(_queryImpl->getById(_escNs, ESCCollection::generateId(getTestESCToken(element), 1))
                     .isEmpty());

    doSingleDelete(2);
    doSingleDelete(1);

    assertDocumentCounts(0, 2, 2, 4);

    ASSERT_ECC_DOC(element, 1, 2, 2);
    ASSERT_ECC_DOC(element, 2, 1, 1);
}

// Insert two documents with different values and delete them
TEST_F(FleCrudTest, InsertTwoDifferentAndDeleteTwo) {

    doSingleInsert(1,
                   BSON("encrypted"
                        << "secret"));
    doSingleInsert(2,
                   BSON("encrypted"
                        << "topsecret"));

    assertDocumentCounts(2, 2, 0, 2);

    doSingleDelete(2);
    doSingleDelete(1);

    assertDocumentCounts(0, 2, 2, 4);

    ASSERT_ECC_DOC(BSON("encrypted"
                        << "secret")
                       .firstElement(),
                   1,
                   1,
                   1);
    ASSERT_ECC_DOC(BSON("encrypted"
                        << "topsecret")
                       .firstElement(),
                   1,
                   1,
                   1);
}

// Insert one document but delete another document
TEST_F(FleCrudTest, InsertOneButDeleteAnother) {

    doSingleInsert(1,
                   BSON("encrypted"
                        << "secret"));
    assertDocumentCounts(1, 1, 0, 1);

    doSingleDelete(2);

    assertDocumentCounts(1, 1, 0, 1);
}

// Update one document
TEST_F(FleCrudTest, UpdateOne) {

    doSingleInsert(1,
                   BSON("encrypted"
                        << "secret"));

    assertDocumentCounts(1, 1, 0, 1);

    doSingleUpdate(1,
                   BSON("encrypted"
                        << "top secret"));

    assertDocumentCounts(1, 2, 1, 3);

    validateDocument(1,
                     BSON("_id" << 1 << "counter" << 2 << "plainText"
                                << "sample"
                                << "encrypted"
                                << "top secret"));
}

// Update one document but to the same value
TEST_F(FleCrudTest, UpdateOneSameValue) {

    doSingleInsert(1,
                   BSON("encrypted"
                        << "secret"));

    assertDocumentCounts(1, 1, 0, 1);

    doSingleUpdate(1,
                   BSON("encrypted"
                        << "secret"));

    assertDocumentCounts(1, 2, 1, 3);

    validateDocument(1,
                     BSON("_id" << 1 << "counter" << 2 << "plainText"
                                << "sample"
                                << "encrypted"
                                << "secret"));
}

// Rename safeContent
TEST_F(FleCrudTest, RenameSafeContent) {

    doSingleInsert(1,
                   BSON("encrypted"
                        << "secret"));

    assertDocumentCounts(1, 1, 0, 1);

    BSONObjBuilder builder;
    builder.append("$inc", BSON("counter" << 1));
    builder.append("$rename", BSON(kSafeContent << "foo"));
    auto result = builder.obj();

    ASSERT_THROWS_CODE(doSingleUpdateWithUpdateDoc(1, result), DBException, 6371506);
}

// Mess with __safeContent__ and ensure the update errors
TEST_F(FleCrudTest, SetSafeContent) {
    doSingleInsert(1,
                   BSON("encrypted"
                        << "secret"));

    assertDocumentCounts(1, 1, 0, 1);

    BSONObjBuilder builder;
    builder.append("$inc", BSON("counter" << 1));
    builder.append("$set", BSON(kSafeContent << "foo"));
    auto result = builder.obj();

    ASSERT_THROWS_CODE(doSingleUpdateWithUpdateDoc(1, result), DBException, 6371507);
}

}  // namespace
}  // namespace mongo