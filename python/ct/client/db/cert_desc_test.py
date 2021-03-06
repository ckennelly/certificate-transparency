#!/usr/bin/env python
# coding=utf-8
import unittest

import time
import sys
from ct.client.db import cert_desc
from ct.crypto import cert
from ct.test import test_config
from ct.test import time_utils
import gflags

CERT = cert.Certificate.from_der_file(
        test_config.get_test_file_path("google_cert.der"))
CA_CERT = cert.Certificate.from_pem_file(
        test_config.get_test_file_path("verisign_intermediate.pem"))
DSA_SHA256_CERT = cert.Certificate.from_der_file(
        test_config.get_test_file_path("dsa_with_sha256.der"))
BAD_UTF8_CERT = cert.Certificate.from_pem_file(
        test_config.get_test_file_path("cert_bad_utf8_subject.pem"))
DOMAIN_IN_ISSUER_CERT = cert.Certificate.from_pem_file(
        test_config.get_test_file_path("domain_in_issuer.pem"))
DOMAIN_IN_O_COMPONENT = cert.Certificate.from_pem_file(
        test_config.get_test_file_path("domain_in_o_component.pem"))

class CertificateDescriptionTest(unittest.TestCase):
    def assert_description_subject_matches_source(self, proto, source):
        subject = [(att.type, att.value) for att in proto.subject]
        cert_subject = [(type_.short_name,
                     cert_desc.to_unicode('.'.join(
                             cert_desc.process_name(value.human_readable()))))
                    for type_, value in source.subject()]
        self.assertItemsEqual(cert_subject, subject)

    def assert_description_issuer_matches_source(self, proto, source):
        issuer = [(att.type, att.value) for att in proto.issuer]
        cert_issuer = [(type_.short_name,
                     cert_desc.to_unicode('.'.join(
                             cert_desc.process_name(value.human_readable()))))
                    for type_, value in source.issuer()]
        self.assertItemsEqual(cert_issuer, issuer)

    def assert_description_alt_subject_names_match_source(self, proto, source):
        subject_alternative_names = [(att.type, att.value)
                                     for att in proto.subject_alternative_names]
        cert_subject_alternative_names = [(san.component_key(),
                                           cert_desc.to_unicode('.'.join(
                                            cert_desc.process_name(
                                     san.component_value().human_readable()))))
                    for san in source.subject_alternative_names()]
        self.assertItemsEqual(cert_subject_alternative_names,
                              subject_alternative_names)

    def assert_description_serial_number_matches_source(self, proto, source):
        self.assertEqual(proto.serial_number,
                         str(source.serial_number().human_readable()
                             .upper().replace(':', '')))

    def assert_description_validity_dates_match_source(self, proto, source):
        self.assertEqual(time.gmtime(proto.validity.not_before / 1000),
                         source.not_before())
        self.assertEqual(time.gmtime(proto.validity.not_after / 1000),
                         source.not_after())

    def assert_description_signature_matches_source(self, proto, source):
        self.assertEqual(proto.tbs_signature.algorithm_id,
                         source.signature()["algorithm"].long_name)
        self.assertEqual(proto.cert_signature.algorithm_id,
                         source.signature_algorithm()["algorithm"].long_name)
        self.assertEqual(proto.tbs_signature.algorithm_id,
                         proto.cert_signature.algorithm_id)

        if source.signature()["parameters"]:
            self.assertEqual(proto.tbs_signature.parameters,
                             source.signature()["parameters"])
        else:
            self.assertFalse(proto.tbs_signature.HasField('parameters'))

        if source.signature_algorithm()["parameters"]:
            self.assertEqual(proto.cert_signature.parameters,
                             source.signature_algorithm()["parameters"])
        else:
            self.assertFalse(proto.cert_signature.HasField('parameters'))

        self.assertEqual(proto.tbs_signature.parameters,
                         proto.cert_signature.parameters)

    def assert_description_matches_source(self, source, expect_ca_true):
        proto = cert_desc.from_cert(source)

        self.assertEqual(proto.der, source.to_der())
        self.assert_description_subject_matches_source(proto, source)
        self.assert_description_issuer_matches_source(proto, source)
        self.assert_description_alt_subject_names_match_source(proto, source)
        self.assertEqual(proto.version, str(source.version().value))
        self.assert_description_serial_number_matches_source(proto, source)
        self.assert_description_validity_dates_match_source(proto, source)
        self.assert_description_signature_matches_source(proto, source)
        self.assertEqual(proto.basic_constraint_ca, expect_ca_true)

    def test_from_cert(self):
        cert = CERT
        is_ca_cert = False

        with time_utils.timezone("UTC"):
            self.assert_description_matches_source(cert, is_ca_cert)

        # Test in non-UTC timezones, to detect timezone issues
        with time_utils.timezone("America/Los_Angeles"):
            self.assert_description_matches_source(cert, is_ca_cert)

        with time_utils.timezone("Asia/Shanghai"):
            self.assert_description_matches_source(cert, is_ca_cert)

    def test_from_cert_with_dsa_sha256_cert(self):
        cert = DSA_SHA256_CERT
        is_ca_cert = False

        with time_utils.timezone("UTC"):
            self.assert_description_matches_source(cert, is_ca_cert)

        # Test in non-UTC timezones, to detect timezone issues
        with time_utils.timezone("America/Los_Angeles"):
            self.assert_description_matches_source(cert, is_ca_cert)

        with time_utils.timezone("Asia/Shanghai"):
            self.assert_description_matches_source(cert, is_ca_cert)

    def test_from_cert_with_ca_cert(self):
        cert = CA_CERT
        is_ca_cert = True

        with time_utils.timezone("UTC"):
            self.assert_description_matches_source(cert, is_ca_cert)

        # Test in non-UTC timezones, to detect timezone issues
        with time_utils.timezone("America/Los_Angeles"):
            self.assert_description_matches_source(cert, is_ca_cert)

        with time_utils.timezone("Asia/Shanghai"):
            self.assert_description_matches_source(cert, is_ca_cert)

    def test_cert_with_mangled_utf8(self):
        cert = BAD_UTF8_CERT
        is_ca_cert = False

        with time_utils.timezone("UTC"):
            self.assert_description_matches_source(cert, is_ca_cert)

        # Test in non-UTC timezones, to detect timezone issues
        with time_utils.timezone("America/Los_Angeles"):
            self.assert_description_matches_source(cert, is_ca_cert)

        with time_utils.timezone("Asia/Shanghai"):
            self.assert_description_matches_source(cert, is_ca_cert)

        proto = cert_desc.from_cert(cert)

    def test_does_not_reverse_domain_names_in_ou(self):
        ou = [t for t in cert_desc.from_cert(DOMAIN_IN_ISSUER_CERT).issuer
              if t.type == "OU"][0]
        self.assertEqual("www.digicert.com", ou.value)

    def test_does_not_reverse_domain_names_in_o_component(self):
        ou = [t for t in cert_desc.from_cert(DOMAIN_IN_O_COMPONENT).subject
              if t.type == "O"][0]
        self.assertEqual("go-greenevents.com", ou.value)

    def test_process_value(self):
        self.assertEqual(["London"], cert_desc.process_name("London"))
        self.assertEqual(["Bob Smith"], cert_desc.process_name("Bob Smith"))
        self.assertEqual(["com", "googleapis", "ct"],
                         cert_desc.process_name("ct.googleapis.com"))
        self.assertEqual(["com", "github"],
                         cert_desc.process_name("gItHuB.CoM"))
        # These two are unfortunate outcomes:
        # 1. single-word hostnames are indistinguishable from single-word CN
        # terms like State, City, Organization
        self.assertEqual(["LOCALhost"], cert_desc.process_name("LOCALhost"))
        # 2. IP addresses should perhaps not be reversed like hostnames are
        self.assertEqual(["1", "0", "168", "192"],
                         cert_desc.process_name("192.168.0.1"))

    def test_to_unicode(self):
        self.assertEqual(u"foobar", cert_desc.to_unicode("foobar"))
        # The given string is encoded using ISO-8859-1, not UTF-8.
        # Assuming it is UTF-8 yields invalid Unicode \uDBE0.
        self.assertNotEqual(u"R\uDBE0S", cert_desc.to_unicode("R\xED\xAF\xA0S"))
        # Detecting the failure and retrying as ISO-8859-1.
        self.assertEqual(u"R\u00ED\u00AF\u00A0S",
                         cert_desc.to_unicode("R\xED\xAF\xA0S"))


if __name__ == "__main__":
    sys.argv = gflags.FLAGS(sys.argv)
    unittest.main()
