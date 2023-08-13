#include <librats/api.h>
#include <openssl/pem.h>
#include <openssl/evp.h>
#include <unistd.h>
#include <ctype.h>

int generate_key_pairs(uint8_t **private_key_out, size_t *private_key_size_out)
{
	EC_KEY *eckey = NULL;
	EVP_PKEY *pkey = NULL;

	BIO *bio = NULL;
	BUF_MEM *bptr = NULL;

	uint8_t *private_key = NULL;
	long private_key_size;

	int ret = -1;

	/* Generate private key and public key */
	eckey = EC_KEY_new_by_curve_name(NID_X9_62_prime256v1);
	if (!eckey)
		goto err;
	EC_KEY_set_asn1_flag(eckey, OPENSSL_EC_NAMED_CURVE);

	if (!EC_KEY_generate_key(eckey))
		goto err;

	if (!EC_KEY_check_key(eckey))
		goto err;

	pkey = EVP_PKEY_new();
	EVP_PKEY_assign_EC_KEY(pkey, eckey);
	eckey = NULL;

	/* Encode private key */
	bio = BIO_new(BIO_s_mem());
	if (!bio)
		goto err;

	if (!PEM_write_bio_PrivateKey(bio, pkey, NULL, NULL, 0, 0, NULL))
		goto err;

	private_key_size = BIO_get_mem_data(bio, &private_key);
	if (private_key_size <= 0)
		goto err;

	BIO_get_mem_ptr(bio, &bptr);
	(void)BIO_set_close(bio, BIO_NOCLOSE);
	BIO_free(bio);
	bio = NULL;
	bptr->data = NULL;
	BUF_MEM_free(bptr);
	bptr = NULL;

	/* Set function output */
	*private_key_out = private_key;
	private_key = NULL;
	*private_key_size_out = private_key_size;

	ret = 0;
err:
	if (private_key)
		free(private_key);
	if (bio)
		BIO_free(bio);
	if (bptr)
		BUF_MEM_free(bptr);
	if (pkey)
		EVP_PKEY_free(pkey);
	if (eckey)
		EC_KEY_free(eckey);
	if (ret)
		printf("Failed to generate private key\n");
	return ret;
}

void print_claim_value(uint8_t *value, size_t value_size)
{
	bool hex = false;
	for (size_t i = 0; i < value_size; ++i) {
		if (!isprint(value[i])) {
			hex = true;
			break;
		}
	}
	if (hex) {
		printf("(hex)");
		for (size_t i = 0; i < value_size; ++i) {
			printf("%02X", value[i]);
		}
	} else {
		printf("'%.*s'", (int)value_size, value);
	}
}

int verify_callback(claim_t *claims, size_t claims_size, void *args_in)
{
	int ret = 0;
	printf("----------------------------------------\n");
	printf("verify_callback called, claims %p, claims_size %zu, args %p\n", claims, claims_size,
	       args_in);
	for (size_t i = 0; i < claims_size; ++i) {
		printf("claims[%zu] -> name: '%s' value_size: %zu value: ", i, claims[i].name,
		       claims[i].value_size);
		print_claim_value(claims[i].value, claims[i].value_size);
		printf("\n");
	}

	/* Let's check all custom claims exits and unchanged */
	typedef struct {
		const claim_t *custom_claims;
		size_t custom_claims_size;
	} args_t;
	args_t *args = (args_t *)args_in;

	printf("checking for all %zu user-defined custom claims\n", args->custom_claims_size);

	for (size_t i = 0; i < args->custom_claims_size; ++i) {
		const claim_t *claim = &args->custom_claims[i];
		bool found = false;
		for (size_t j = 0; j < claims_size; ++j) {
			if (!strcmp(claim->name, claims[j].name)) {
				found = true;
				if (claim->value_size != claims[j].value_size) {
					printf("different claim detected -> name: '%s' expected value_size: %zu got: %zu\n",
					       claim->name, claim->value_size,
					       claims[j].value_size);
					ret = 1;
					break;
				}

				if (memcmp(claim->value, claims[j].value, claim->value_size)) {
					printf("different claim detected -> name: '%s' value_size: %zu expected value: ",
					       claim->name, claim->value_size);
					print_claim_value(claim->value, claim->value_size);
					printf(" got: ");
					print_claim_value(claims[j].value, claim->value_size);
					printf("\n");
					ret = 1;
					break;
				}
				break;
			}
		}
		if (!found) {
			printf("different claim detected -> name: '%s' not found\n", claim->name);
			ret = 1;
		}
	}
	printf("verify_callback check result:\t%s\n", ret == 0 ? "SUCCESS" : "FAILED");
	printf("----------------------------------------\n");
	return ret;
}

int get_attestation_certificate(rats_conf_t conf, bool no_privkey, const claim_t *custom_claims,
				size_t custom_claims_size, uint8_t **certificate_out,
				size_t *certificate_size_out)
{
	uint8_t *private_key = NULL;
	size_t private_key_size = 0;

	int ret = -1;
	rats_attester_err_t rats_ret;

	if (no_privkey) {
		printf("The flag no_privkey is true. We will let librats to generate random key pairs.\n");
	} else {
		printf("The flag no_privkey is false. Now generate key pairs for librats.\n");
		/* Generate private key and public key */
		if (generate_key_pairs(&private_key, &private_key_size) < 0)
			goto err;
	}

	rats_cert_subject_t subject_name = { .organization = "Inclavare Containers",
					     .common_name = "LibRATS" };

	printf("\nGenerate certificate with librats now ...\n");
	/* Collect certificate */
	rats_ret = librats_get_attestation_certificate(conf, subject_name, &private_key,
						       &private_key_size, custom_claims,
						       custom_claims_size, true, certificate_out,
						       certificate_size_out);
	if (rats_ret != RATS_ATTESTER_ERR_NONE) {
		printf("Failed to generate certificate %#x\n", rats_ret);
		goto err;
	}

	if (no_privkey) {
		printf("----------------------------------------\n");
		printf("The privkey generated by librats (PEM format):\n");
		printf("privkey len: %zu\n", private_key_size);
		printf("privkey: \n%.*s\n",
		       private_key[private_key_size - 1] == '\n' ? (int)private_key_size - 1 :
								   (int)private_key_size,
		       private_key);
		printf("----------------------------------------\n");
	}

	ret = 0;
err:
	if (private_key)
		free(private_key);
	return ret;
}

int verify_attestation_certificate(rats_conf_t conf, uint8_t *certificate, size_t certificate_size,
				   void *args)
{
	int ret = -1;
	rats_verifier_err_t rats_ret;

	printf("\nVerify certificate with librats now ...\n");

	/* Verify certificate */
	rats_ret = librats_verify_attestation_certificate(conf, certificate, certificate_size,
							  verify_callback, args);
	if (rats_ret != RATS_VERIFIER_ERR_NONE) {
		printf("Failed to verify certificate %#x\n", rats_ret);
		goto err;
	}

	ret = 0;
err:
	return ret;
}