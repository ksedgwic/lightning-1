#include <iostream>

#define __STDC_FORMAT_MACROS
#include <inttypes.h>

#include <grpc++/grpc++.h>

#include "contrib/remote_hsmd/api.pb.h"
#include "contrib/remote_hsmd/api.grpc.pb.h"

#include "contrib/remote_hsmd/proxy.h"

extern "C" {
#include <bitcoin/chainparams.h>
#include <bitcoin/privkey.h>
#include <bitcoin/tx.h>
#include <common/node_id.h>
#include <common/status.h>
#include <common/utils.h>
#include <common/utxo.h>
}

using std::cerr;
using std::endl;
using std::string;
using std::unique_ptr;

using grpc::Channel;
using grpc::ClientContext;
using grpc::Status;
using grpc::StatusCode;

using rpc::ECDHReq;
using rpc::ECDHRsp;
using rpc::InitHSMReq;
using rpc::InitHSMRsp;
using rpc::KeyLocator;
using rpc::SignDescriptor;
using rpc::SignWithdrawalTxReq;
using rpc::SignWithdrawalTxRsp;
using rpc::Signature;
using rpc::Signer;

using ::google::protobuf::RepeatedPtrField;

namespace {
unique_ptr<Signer::Stub> stub;
string last_message;
struct node_id self_id;

proxy_stat map_status(StatusCode const & code)
{
	switch (code) {
	case StatusCode::OK:			return PROXY_OK;
	case StatusCode::DEADLINE_EXCEEDED:	return PROXY_TIMEOUT;
	case StatusCode::UNAVAILABLE:		return PROXY_UNAVAILABLE;
	case StatusCode::INVALID_ARGUMENT:	return PROXY_INVALID_ARGUMENT;
	case StatusCode::INTERNAL:		return PROXY_INTERNAL_ERROR;
	default:
		cerr << "UNHANDLED grpc::StatusCode " << int(code) << endl;
		abort();
	}
}

/* type_to_string has issues in the C++ environment, use this to
   dump binary data as hex instead. */
string as_hex(const void *vptr, size_t sz)
{
	static const char hex[] = "0123456789abcdef";
	string retval(sz*2, '\0');
	uint8_t const * ptr = (uint8_t const *) vptr;
	for (size_t ii = 0; ii < sz; ++ii) {
		retval[ii*2+0] = hex[(*ptr) >> 4];
		retval[ii*2+1] = hex[(*ptr) & 0xf];
		ptr++;
	}
	return retval;
}

string channel_nonce(struct node_id *peer_id, u64 dbid)
{
	return string((char const *)peer_id->k, sizeof(peer_id->k)) +
		string((char const *)&dbid, sizeof(dbid));
}

u8 ***return_sigs(RepeatedPtrField< ::rpc::Signature > const &isigs)
{
	u8 ***osigs;
	int nsigs = isigs.size();
	osigs = tal_arrz(tmpctx, u8**, nsigs);
	for (size_t ii = 0; ii < nsigs; ++ii) {
		Signature const &sig = isigs[ii];
		int nelem = sig.item_size();
		osigs[ii] = tal_arrz(osigs, u8*, nelem);
		for (size_t jj = 0; jj < nelem; ++jj) {
			string const &elem = sig.item(jj);
			size_t elen = elem.size();
			osigs[ii][jj] = tal_arr(osigs[ii], u8, elen);
			memcpy(osigs[ii][jj], &elem[0], elen);
		}
	}
	return osigs;
}

/* BIP144:
 * If the witness is empty, the old serialization format should be used. */
bool uses_witness(const struct bitcoin_tx *tx)
{
	size_t i;
	for (i = 0; i < tx->wtx->num_inputs; i++) {
		if (tx->wtx->inputs[i].witness)
			return true;
	}
	return false;
}

string serialized_tx(struct bitcoin_tx *tx, bool bip144)
{
	int res;
	size_t len, written;
	u8 *serialized;;
	u8 flag = 0;

	if (bip144 && uses_witness(tx))
		flag |= WALLY_TX_FLAG_USE_WITNESS;

	res = wally_tx_get_length(tx->wtx, flag, &len);
	assert(res == WALLY_OK);

	string retval(len, '\0');
	res = wally_tx_to_bytes(tx->wtx, flag, (unsigned char *)&retval[0],
				retval.size(), &written);
	assert(res == WALLY_OK);
	assert(len == written);
	return retval;
}

} /* end namespace */

extern "C" {
const char *proxy_last_message(void)
{
	return last_message.c_str();
}

void proxy_setup()
{
	status_debug("%s:%d %s", __FILE__, __LINE__, __FUNCTION__);
	auto channel = grpc::CreateChannel("localhost:50051",
					   grpc::InsecureChannelCredentials());
	stub = Signer::NewStub(channel);
	last_message = "";
}

proxy_stat proxy_init_hsm(struct bip32_key_version *bip32_key_version,
			  struct chainparams const *chainparams,
			  struct secret *hsm_encryption_key,
			  struct privkey *privkey,
			  struct secret *seed,
			  struct secrets *secrets,
			  struct sha256 *shaseed,
			  struct secret *hsm_secret,
			  struct node_id *o_node_id)
{
	status_debug("%s:%d %s", __FILE__, __LINE__, __FUNCTION__);
	last_message = "";
	InitHSMReq req;

	auto kv = req.mutable_key_version();
	kv->set_pubkey_version(bip32_key_version->bip32_pubkey_version);
	kv->set_privkey_version(bip32_key_version->bip32_privkey_version);

	auto cp = req.mutable_chainparams();
	cp->set_network_name(chainparams->network_name);
	cp->set_bip173_name(chainparams->bip173_name);
	cp->set_bip70_name(chainparams->bip70_name);
	cp->set_genesis_blockhash(
		&chainparams->genesis_blockhash.shad.sha.u.u8,
		sizeof(chainparams->genesis_blockhash.shad.sha.u.u8));
	cp->set_rpc_port(chainparams->rpc_port);
	cp->set_cli(chainparams->cli);
	cp->set_cli_args(chainparams->cli_args);
	cp->set_cli_min_supported_version(
		chainparams->cli_min_supported_version);
	cp->set_dust_limit_sat(chainparams->dust_limit.satoshis);
	cp->set_max_funding_sat(chainparams->max_funding.satoshis);
	cp->set_max_payment_msat(chainparams->max_payment.millisatoshis);
	cp->set_when_lightning_became_cool(
		chainparams->when_lightning_became_cool);
	cp->set_p2pkh_version(chainparams->p2pkh_version);
	cp->set_p2sh_version(chainparams->p2sh_version);
	cp->set_testnet(chainparams->testnet);

	auto kv2 = cp->mutable_bip32_key_version();
	kv2->set_pubkey_version(
		chainparams->bip32_key_version.bip32_pubkey_version);
	kv2->set_privkey_version(
		chainparams->bip32_key_version.bip32_privkey_version);

	cp->set_is_elements(chainparams->is_elements);
	cp->set_fee_asset_tag(&chainparams->fee_asset_tag,
			      sizeof(chainparams->fee_asset_tag));

	/* FIXME - Sending the secret instead of generating on the remote. */
	req.set_hsm_secret(hsm_secret->data, sizeof(hsm_secret->data));

	ClientContext context;
	InitHSMRsp rsp;
	Status status = stub->InitHSM(&context, req, &rsp);
	if (status.ok()) {
		assert(rsp.self_node_id().length() == sizeof(o_node_id->k));
		memcpy(o_node_id->k, rsp.self_node_id().c_str(),
		       sizeof(o_node_id->k));
		assert(rsp.self_node_id().length() == sizeof(self_id.k));
		memcpy(self_id.k, rsp.self_node_id().c_str(),
		       sizeof(self_id.k));
		status_debug("%s:%d %s node_id=%s",
			     __FILE__, __LINE__, __FUNCTION__,
			     as_hex(o_node_id->k,
				    sizeof(o_node_id->k)).c_str());
		last_message = "success";
		return PROXY_OK;
	} else {
		status_unusual("%s:%d %s: %s",
			       __FILE__, __LINE__, __FUNCTION__,
			       status.error_message().c_str());
		last_message = status.error_message();
		return map_status(status.error_code());
	}
}

proxy_stat proxy_handle_ecdh(struct pubkey *point,
			     struct secret *o_ss)
{
	status_debug(
		"%s:%d %s self_id=%s point=%s",
		__FILE__, __LINE__, __FUNCTION__,
		as_hex(self_id.k, sizeof(self_id.k)).c_str(),
		as_hex(point->pubkey.data, sizeof(point->pubkey.data)).c_str()
		);
	last_message = "";
	ECDHReq req;

	req.set_self_node_id((const char *) self_id.k, sizeof(self_id.k));
	req.set_point((const char *) point->pubkey.data,
		      sizeof(point->pubkey.data));

	ClientContext context;
	ECDHRsp rsp;
	Status status = stub->ECDH(&context, req, &rsp);
	if (status.ok()) {
		assert(rsp.shared_secret().length() == sizeof(o_ss->data));
		memcpy(o_ss->data, rsp.shared_secret().c_str(),
		       sizeof(o_ss->data));
		status_debug("%s:%d %s self_id=%s ss=%s",
			     __FILE__, __LINE__, __FUNCTION__,
			     as_hex(self_id.k, sizeof(self_id.k)).c_str(),
			     as_hex(o_ss->data, sizeof(o_ss->data)).c_str());
		last_message = "success";
		return PROXY_OK;
	} else {
		status_unusual("%s:%d %s: self_id=%s %s",
			       __FILE__, __LINE__, __FUNCTION__,
			       as_hex(self_id.k, sizeof(self_id.k)).c_str(),
			       status.error_message().c_str());
		last_message = status.error_message();
		return map_status(status.error_code());
	}
}

proxy_stat proxy_handle_sign_withdrawal_tx(
	struct node_id *peer_id,
	u64 dbid,
	struct amount_sat *satoshi_out,
	struct amount_sat *change_out,
	u32 change_keyindex,
	struct bitcoin_tx_output **outputs,
	struct utxo **utxos,
	struct bitcoin_tx *tx,
	u8 ****o_sigs)
{
	status_debug(
		"%s:%d %s self_id=%s peer_id=%s dbid=%" PRIu64 " "
		"satoshi_out=%" PRIu64 " change_out=%" PRIu64 " "
		"change_keyindex=%u",
		__FILE__, __LINE__, __FUNCTION__,
		as_hex(self_id.k, sizeof(self_id.k)).c_str(),
		as_hex(peer_id->k, sizeof(peer_id->k)).c_str(),
		dbid,
		satoshi_out->satoshis,
		change_out->satoshis,
		change_keyindex
		);
	last_message = "";
	SignWithdrawalTxReq req;
	req.set_self_node_id((const char *) self_id.k, sizeof(self_id.k));
	req.set_channel_nonce(channel_nonce(peer_id, dbid));
	req.set_raw_tx_bytes(serialized_tx(tx, true));

	assert(tx->wtx->num_inputs == tal_count(utxos));
	for (size_t ii = 0; ii < tal_count(utxos); ii++) {
	 	const struct utxo *in = utxos[ii];
		SignDescriptor *desc = req.add_input_descs();
		desc->mutable_key_loc()->set_key_index(in->keyindex);
		desc->mutable_key_loc()->set_key_family(KeyLocator::layer_one);
		desc->mutable_output()->set_value(in->amount.satoshis);
	}

	/* FIXME - Why doesn't this assert succeed?
        contrib/remote_hsmd/proxy.cc:310 proxy_handle_sign_withdrawal_tx num_outputs=2 tal_count(outputs)=1
￼	fprintf(stderr, "%s:%d %s num_outputs=%d tal_count(outputs)=%d\n",
		__FILE__, __LINE__, __FUNCTION__,
		tx->wtx->num_outputs, tal_count(outputs));
	assert(tx->wtx->num_outputs == tal_count(outputs));
	*/

	for (size_t ii = 0; ii < tal_count(outputs); ii++) {
	 	const struct bitcoin_tx_output *out = outputs[ii];
		SignDescriptor *desc = req.add_output_descs();
		if (out->script) {
			/* FIXME - This assert fails too:
                        contrib/remote_hsmd/proxy.cc:323 proxy_handle_sign_withdrawal_tx out->amount.satoshis=1000000 change_out->satoshis=995418
			fprintf(stderr,
				"%s:%d %s out->amount.satoshis=%" PRIu64 " "
				"change_out->satoshis=%" PRIu64 "\n",
				__FILE__, __LINE__, __FUNCTION__,
				out->amount.satoshis, change_out->satoshis);
			assert(out->amount.satoshis == change_out->satoshis);
                        */
			desc->mutable_key_loc()->set_key_index(change_keyindex);
			desc->mutable_key_loc()->set_key_family(
				KeyLocator::layer_one);
		} else {
			desc->mutable_key_loc()->set_key_family(
				KeyLocator::unknown);
		}
	}

	ClientContext context;
	SignWithdrawalTxRsp rsp;
	Status status = stub->SignWithdrawalTx(&context, req, &rsp);
	if (status.ok()) {
		*o_sigs = return_sigs(rsp.sigs());
		status_debug("%s:%d %s self_id=%s",
			     __FILE__, __LINE__, __FUNCTION__,
			     as_hex(self_id.k, sizeof(self_id.k)).c_str());
		last_message = "success";
		return PROXY_OK;
	} else {
		status_unusual("%s:%d %s: self_id=%s %s",
			       __FILE__, __LINE__, __FUNCTION__,
			       as_hex(self_id.k, sizeof(self_id.k)).c_str(),
			       status.error_message().c_str());
		last_message = status.error_message();
		return map_status(status.error_code());
	}
}

} /* extern "C" */

