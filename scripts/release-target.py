#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Validate a release target without replacing an existing tag or release."""
import argparse
import json
import os
import re
import sys
import urllib.error
import urllib.parse
import urllib.request

TAG = re.compile(r"v(?:0|[1-9][0-9]*)\.(?:0|[1-9][0-9]*)\.(?:0|[1-9][0-9]*)")
SHA = re.compile(r"[0-9a-fA-F]{40}")
MISSING = object()


class ApiError(RuntimeError):
    def __init__(self, message, status=None):
        super().__init__(message)
        self.status = status


class NoRedirect(urllib.request.HTTPRedirectHandler):
    def redirect_request(self, request, file, code, message, headers, new_url):
        return None


class GitHub:
    def __init__(self, repository, token, api_url="https://api.github.com"):
        if not re.fullmatch(r"[A-Za-z0-9_.-]+/[A-Za-z0-9_.-]+", repository or ""):
            raise ValueError("GITHUB_REPOSITORY must be owner/repository")
        if any(part in (".", "..") for part in repository.split("/")):
            raise ValueError("Invalid repository path")
        parsed = urllib.parse.urlsplit(api_url)
        if (parsed.scheme != "https" or not parsed.netloc or parsed.username or
                parsed.password or parsed.query or parsed.fragment):
            raise ValueError("GITHUB_API_URL must be an HTTPS API base URL")
        if not token or "\n" in token or "\r" in token:
            raise ValueError("GH_TOKEN is required")
        self.base = api_url.rstrip("/") + "/repos/" + repository
        self.token = token
        self.opener = urllib.request.build_opener(NoRedirect())

    def request(self, method, path, payload=None):
        data = None if payload is None else json.dumps(payload).encode("utf-8")
        request = urllib.request.Request(
            self.base + path, data=data, method=method,
            headers={"Accept": "application/vnd.github+json",
                     "Authorization": "Bearer " + self.token,
                     "X-GitHub-Api-Version": "2022-11-28",
                     "Content-Type": "application/json",
                     "User-Agent": "cloudtune-nx-release-target"},
        )
        try:
            with self.opener.open(request, timeout=30) as response:
                expected = 201 if method == "POST" else 200
                if response.status != expected:
                    raise ApiError(f"{method} {path}: unexpected HTTP {response.status}",
                                   response.status)
                result = json.load(response)
                if not isinstance(result, (dict, list)):
                    raise ValueError("Expected a JSON object or array")
                return result
        except urllib.error.HTTPError as error:
            raise ApiError(f"{method} {path}: HTTP {error.code}", error.code) from None
        except (OSError, urllib.error.URLError, ValueError) as error:
            raise ApiError(f"{method} {path}: network or invalid JSON response") from None


def optional_get(api, path):
    try:
        return api.request("GET", path)
    except ApiError as error:
        if error.status == 404:
            return MISSING
        raise


def no_release(api, tag):
    # The tag endpoint alone is insufficient: draft releases are listed only
    # for authenticated identities with push access to the repository.
    if optional_get(api, "/releases/tags/" + tag) is not MISSING:
        raise ValueError(f"A release already exists for {tag}")
    for page in range(1, 1001):
        releases = api.request("GET", f"/releases?per_page=100&page={page}")
        if not isinstance(releases, list) or len(releases) > 100:
            raise ValueError("Invalid release listing")
        for release in releases:
            if not isinstance(release, dict) or not isinstance(release.get("tag_name"), str):
                raise ValueError("Invalid release listing entry")
            if release["tag_name"] == tag:
                raise ValueError(f"A published or draft release already exists for {tag}")
        if len(releases) < 100:
            return
    raise ValueError("Release pagination limit reached; cannot prove target is unused")


def object_target(value):
    target = value.get("object") if isinstance(value, dict) else None
    if (not isinstance(target, dict) or target.get("type") not in ("commit", "tag") or
            not isinstance(target.get("sha"), str) or not SHA.fullmatch(target["sha"])):
        raise ValueError("Invalid tag target; expected a commit or annotated tag object")
    return target["type"], target["sha"].lower()


def tag_commit(api, tag):
    reference = optional_get(api, "/git/ref/tags/" + tag)
    if reference is MISSING:
        return None
    if not isinstance(reference, dict) or reference.get("ref") != "refs/tags/" + tag:
        raise ValueError("API returned a different or malformed tag reference")
    kind, sha = object_target(reference)
    visited = set()
    for _ in range(8):
        if kind == "commit":
            return sha
        if sha in visited:
            raise ValueError("Annotated tag cycle")
        visited.add(sha)
        # Build paths from validated hashes, never follow server-supplied URLs.
        kind, sha = object_target(api.request("GET", "/git/tags/" + sha))
    if kind == "commit":
        return sha
    raise ValueError("Annotated tag nesting exceeds eight objects")


def check_target(api, tag, commit, *, allow_missing_tag=False, create_tag=False,
                 verify_tag_only=False):
    if not TAG.fullmatch(tag):
        raise ValueError("Tag must be exactly vX.Y.Z without leading zeros")
    if not SHA.fullmatch(commit):
        raise ValueError("Commit must be a full 40-character hexadecimal SHA")
    if sum((allow_missing_tag, create_tag, verify_tag_only)) > 1:
        raise ValueError("Target modes are mutually exclusive")
    commit = commit.lower()
    if not verify_tag_only:
        no_release(api, tag)
    actual = tag_commit(api, tag)
    if actual is not None:
        if actual != commit:
            raise ValueError(f"{tag} points to another commit; refusing to move it")
        return "existing"
    if verify_tag_only or not (allow_missing_tag or create_tag):
        raise ValueError(f"{tag} does not exist")
    # A tag may legally target a blob/tree; require the missing tag's proposed
    # SHA to be an actual commit before either approving or creating it.
    proposed = api.request("GET", "/git/commits/" + commit)
    if (not isinstance(proposed, dict) or not isinstance(proposed.get("sha"), str) or
            proposed["sha"].lower() != commit):
        raise ValueError("Proposed target is not the requested commit")
    if allow_missing_tag:
        return "missing"
    no_release(api, tag)
    try:
        api.request("POST", "/git/refs", {"ref": "refs/tags/" + tag, "sha": commit})
    except ApiError as error:
        if error.status != 422:
            raise
        # A concurrent creator can win. Never PATCH/force the reference; only
        # accept a fresh read proving its exact commit matches ours.
    if tag_commit(api, tag) != commit:
        raise ValueError("Created or concurrent tag does not match the requested commit")
    no_release(api, tag)
    return "created"


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("tag")
    parser.add_argument("--commit", required=True)
    mode = parser.add_mutually_exclusive_group()
    mode.add_argument("--allow-missing-tag", action="store_true")
    mode.add_argument("--create-tag", action="store_true")
    mode.add_argument("--verify-tag-only", action="store_true")
    args = parser.parse_args()
    try:
        api = GitHub(os.environ.get("GITHUB_REPOSITORY"), os.environ.get("GH_TOKEN"),
                     os.environ.get("GITHUB_API_URL", "https://api.github.com"))
        result = check_target(api, args.tag, args.commit,
                              allow_missing_tag=args.allow_missing_tag,
                              create_tag=args.create_tag,
                              verify_tag_only=args.verify_tag_only)
        print(f"Verified {args.tag} at {args.commit.lower()} ({result})")
        return 0
    except (ApiError, ValueError) as error:
        print(f"Release target validation failed: {error}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())
