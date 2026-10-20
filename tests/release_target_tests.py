#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Deterministic fake GitHub API tests; never contact or modify GitHub."""
import importlib.util
import io
import json
from pathlib import Path
import unittest
import urllib.error
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[1]
SPEC = importlib.util.spec_from_file_location("release_target", ROOT / "scripts/release-target.py")
release = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(release)
TAG = "v0.2.2"
SHA = "a" * 40
OTHER = "b" * 40
REF = "/git/ref/tags/" + TAG
NOT_FOUND = release.ApiError("not found", 404)


def reference(sha=SHA, kind="commit"):
    return {"ref": "refs/tags/" + TAG, "object": {"type": kind, "sha": sha}}


def absent_release():
    return [("GET", "/releases/tags/" + TAG, NOT_FOUND),
            ("GET", "/releases?per_page=100&page=1", [])]


def missing_tag():
    return absent_release() + [("GET", REF, NOT_FOUND)]


def create_steps(result=None):
    return (missing_tag() + [("GET", "/git/commits/" + SHA, {"sha": SHA})] +
            absent_release() +
            [("POST", "/git/refs", reference() if result is None else result)])


class FakeAPI:
    def __init__(self, steps):
        self.steps = list(steps)
        self.calls = []

    def request(self, method, path, payload=None):
        self.calls.append((method, path, payload))
        if not self.steps:
            raise AssertionError(f"Unexpected request: {method} {path}")
        expected_method, expected_path, response = self.steps.pop(0)
        if (method, path) != (expected_method, expected_path):
            raise AssertionError(f"Expected {(expected_method, expected_path)}, got {(method, path)}")
        if isinstance(response, Exception):
            raise response
        return response


class TargetTests(unittest.TestCase):
    def test_existing_lightweight_read_only(self):
        api = FakeAPI(absent_release() + [("GET", REF, reference())])
        self.assertEqual(release.check_target(api, TAG, SHA.upper()), "existing")
        self.assertFalse(api.steps)
        self.assertTrue(all(call[0] == "GET" for call in api.calls))

    def test_missing_default_rejected(self):
        api = FakeAPI(missing_tag())
        with self.assertRaisesRegex(ValueError, "does not exist"):
            release.check_target(api, TAG, SHA)
        self.assertTrue(all(call[0] == "GET" for call in api.calls))

    def test_allow_missing_is_read_only_and_requires_commit(self):
        api = FakeAPI(missing_tag() + [("GET", "/git/commits/" + SHA, {"sha": SHA})])
        self.assertEqual(release.check_target(api, TAG, SHA, allow_missing_tag=True), "missing")
        self.assertTrue(all(call[0] == "GET" for call in api.calls))

    def test_missing_proposed_commit_is_not_accepted(self):
        for response in (NOT_FOUND, {"sha": OTHER}, {"sha": None}, []):
            with self.subTest(response=response):
                api = FakeAPI(missing_tag() + [("GET", "/git/commits/" + SHA, response)])
                with self.assertRaises((ValueError, release.ApiError)):
                    release.check_target(api, TAG, SHA, create_tag=True)
                self.assertTrue(all(call[0] == "GET" for call in api.calls))

    def test_existing_release_blocks_all_writes(self):
        for draft in (True, False):
            with self.subTest(draft=draft):
                api = FakeAPI([("GET", "/releases/tags/" + TAG,
                                {"tag_name": TAG, "draft": draft})])
                with self.assertRaisesRegex(ValueError, "release already exists"):
                    release.check_target(api, TAG, SHA, create_tag=True)
                self.assertEqual(len(api.calls), 1)

    def test_draft_on_later_page_blocks_all_writes(self):
        api = FakeAPI([
            ("GET", "/releases/tags/" + TAG, NOT_FOUND),
            ("GET", "/releases?per_page=100&page=1", [{"tag_name": "v0.0.1"}] * 100),
            ("GET", "/releases?per_page=100&page=2", [{"tag_name": TAG, "draft": True}]),
        ])
        with self.assertRaisesRegex(ValueError, "draft release already exists"):
            release.check_target(api, TAG, SHA, create_tag=True)
        self.assertTrue(all(call[0] == "GET" for call in api.calls))

    def test_release_list_failure_is_not_absence(self):
        for response in (NOT_FOUND, release.ApiError("rate limited", 403), None, {},
                         [{"tag_name": None}]):
            with self.subTest(response=response):
                api = FakeAPI([("GET", "/releases/tags/" + TAG, NOT_FOUND),
                               ("GET", "/releases?per_page=100&page=1", response)])
                with self.assertRaises((ValueError, release.ApiError)):
                    release.check_target(api, TAG, SHA, create_tag=True)
                self.assertTrue(all(call[0] == "GET" for call in api.calls))

    def test_http_errors_never_mean_missing(self):
        for status in (301, 401, 403, 409, 422, 429, 500, 503, None):
            for endpoint in ("release", "ref"):
                with self.subTest(status=status, endpoint=endpoint):
                    error = release.ApiError("failure", status)
                    steps = ([("GET", "/releases/tags/" + TAG, error)]
                             if endpoint == "release" else
                             absent_release() + [("GET", REF, error)])
                    api = FakeAPI(steps)
                    with self.assertRaises(release.ApiError):
                        release.check_target(api, TAG, SHA, create_tag=True)
                    self.assertTrue(all(call[0] == "GET" for call in api.calls))

    def test_null_ref_is_invalid_not_missing(self):
        api = FakeAPI(absent_release() + [("GET", REF, None)])
        with self.assertRaises(ValueError):
            release.check_target(api, TAG, SHA, create_tag=True)
        self.assertTrue(all(call[0] == "GET" for call in api.calls))

    def test_existing_mismatch_never_moves_tag(self):
        api = FakeAPI(absent_release() + [("GET", REF, reference(OTHER))])
        with self.assertRaisesRegex(ValueError, "refusing to move"):
            release.check_target(api, TAG, SHA, create_tag=True)
        self.assertTrue(all(call[0] == "GET" for call in api.calls))

    def test_annotated_chain_accepts_eight_objects(self):
        hashes = [f"{i:040x}" for i in range(1, 9)]
        steps = absent_release() + [("GET", REF, reference(hashes[0], "tag"))]
        for index, sha in enumerate(hashes):
            target = reference(hashes[index + 1], "tag") if index < 7 else reference()
            steps.append(("GET", "/git/tags/" + sha, target))
        api = FakeAPI(steps)
        self.assertEqual(release.check_target(api, TAG, SHA), "existing")
        self.assertFalse(api.steps)

    def test_annotated_cycle_rejected(self):
        api = FakeAPI(absent_release() + [("GET", REF, reference(OTHER, "tag")),
                      ("GET", "/git/tags/" + OTHER, reference(OTHER, "tag"))])
        with self.assertRaisesRegex(ValueError, "cycle"):
            release.check_target(api, TAG, SHA)

    def test_annotated_depth_limit(self):
        hashes = [f"{i:040x}" for i in range(1, 10)]
        steps = absent_release() + [("GET", REF, reference(hashes[0], "tag"))]
        steps += [("GET", "/git/tags/" + hashes[i], reference(hashes[i + 1], "tag"))
                  for i in range(8)]
        with self.assertRaisesRegex(ValueError, "eight"):
            release.check_target(FakeAPI(steps), TAG, SHA)

    def test_invalid_tag_objects_rejected(self):
        for value in (reference(SHA, "tree"), reference("../bad", "tag"), {},
                      {"ref": "refs/heads/main", "object": {"type": "commit", "sha": SHA}}):
            with self.subTest(value=value):
                api = FakeAPI(absent_release() + [("GET", REF, value)])
                with self.assertRaises(ValueError):
                    release.check_target(api, TAG, SHA, create_tag=True)
                self.assertTrue(all(call[0] == "GET" for call in api.calls))

    def test_create_uses_only_nonforce_post_and_verifies_readback(self):
        api = FakeAPI(create_steps() + [("GET", REF, reference())] + absent_release())
        self.assertEqual(release.check_target(api, TAG, SHA, create_tag=True), "created")
        writes = [call for call in api.calls if call[0] != "GET"]
        self.assertEqual(writes, [("POST", "/git/refs", {"ref": "refs/tags/" + TAG, "sha": SHA})])
        self.assertFalse(api.steps)

    def test_create_422_race_accepts_only_same_commit(self):
        for target, succeeds in ((reference(), True), (reference(OTHER), False),
                                 (NOT_FOUND, False)):
            with self.subTest(target=target):
                steps = create_steps(release.ApiError("exists", 422)) + [("GET", REF, target)]
                if succeeds:
                    steps += absent_release()
                api = FakeAPI(steps)
                if succeeds:
                    self.assertEqual(release.check_target(api, TAG, SHA, create_tag=True), "created")
                else:
                    with self.assertRaises(ValueError):
                        release.check_target(api, TAG, SHA, create_tag=True)
                self.assertEqual(sum(call[0] != "GET" for call in api.calls), 1)
                self.assertFalse(any(call[0] in ("PATCH", "PUT", "DELETE") for call in api.calls))

    def test_create_other_errors_abort_without_retry(self):
        for code in (404, 403, 500, None):
            with self.subTest(code=code):
                api = FakeAPI(create_steps(release.ApiError("failed", code)))
                with self.assertRaises(release.ApiError):
                    release.check_target(api, TAG, SHA, create_tag=True)
                self.assertFalse(api.steps)

    def test_concurrent_release_rejected_before_post(self):
        api = FakeAPI(missing_tag() + [("GET", "/git/commits/" + SHA, {"sha": SHA}),
                      ("GET", "/releases/tags/" + TAG, {"tag_name": TAG})])
        with self.assertRaises(ValueError):
            release.check_target(api, TAG, SHA, create_tag=True)
        self.assertTrue(all(call[0] == "GET" for call in api.calls))

    def test_verify_only_skips_existing_release_check(self):
        api = FakeAPI([("GET", REF, reference())])
        self.assertEqual(release.check_target(api, TAG, SHA, verify_tag_only=True), "existing")
        self.assertFalse(api.steps)
        with self.assertRaisesRegex(ValueError, "does not exist"):
            release.check_target(FakeAPI([("GET", REF, NOT_FOUND)]), TAG, SHA,
                                 verify_tag_only=True)

    def test_invalid_inputs_make_no_requests(self):
        for tag in ("v01.2.3", "1.2.3", "v1.2", "v1.2.3-rc1", "v1.2.3\n", "../tag"):
            with self.subTest(tag=tag), self.assertRaises(ValueError):
                release.check_target(FakeAPI([]), tag, SHA)
        for sha in ("a" * 39, "z" * 40, SHA + "\n"):
            with self.subTest(sha=sha), self.assertRaises(ValueError):
                release.check_target(FakeAPI([]), TAG, sha)
        with self.assertRaises(ValueError):
            release.check_target(FakeAPI([]), TAG, SHA, create_tag=True, verify_tag_only=True)


class TransportTests(unittest.TestCase):
    def test_network_error_and_malformed_json_are_closed(self):
        api = release.GitHub("owner/repo", "not-a-real-token")
        with patch.object(api.opener, "open", side_effect=urllib.error.URLError("offline")):
            with self.assertRaises(release.ApiError) as failure:
                api.request("GET", "/git/ref/tags/" + TAG)
            self.assertIsNone(failure.exception.status)
        for body in (b"not JSON", b"null", b"true", b"42"):
            with self.subTest(body=body):
                response = io.BytesIO(body)
                response.status = 200
                with patch.object(api.opener, "open", return_value=response):
                    with self.assertRaises(release.ApiError):
                        api.request("GET", "/git/ref/tags/" + TAG)

    def test_request_has_auth_and_post_body(self):
        api = release.GitHub("owner/repo", "not-a-real-token", "https://example.test/api/v3")
        response = io.BytesIO(json.dumps(reference()).encode())
        response.status = 201
        payload = {"ref": "refs/tags/" + TAG, "sha": SHA}
        with patch.object(api.opener, "open", return_value=response) as opener:
            api.request("POST", "/git/refs", payload)
        request = opener.call_args.args[0]
        self.assertEqual(request.full_url, "https://example.test/api/v3/repos/owner/repo/git/refs")
        self.assertEqual(request.get_header("Authorization"), "Bearer not-a-real-token")
        self.assertEqual(json.loads(request.data), payload)
        self.assertEqual(opener.call_args.kwargs["timeout"], 30)

    def test_redirects_are_not_followed(self):
        handler = release.NoRedirect()
        self.assertIsNone(handler.redirect_request(None, None, 302, "", {}, "https://other.test"))

    def test_invalid_configuration(self):
        for repo, token, url in (
            ("../repo", "x", "https://api.github.com"),
            ("owner/repo", "", "https://api.github.com"),
            ("owner/repo", "x", "http://api.github.com"),
            ("owner/repo", "x", "https://user:pass@example.test"),
            ("owner/repo", "x", "https://example.test/?token=secret"),
        ):
            with self.subTest(repo=repo, url=url), self.assertRaises(ValueError):
                release.GitHub(repo, token, url)


if __name__ == "__main__":
    unittest.main()
