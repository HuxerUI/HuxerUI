# Security Policy

HuxerUI is a cross-platform UI framework: application code, the shared runtime, platform backends, the SDK, and the build-time resource tooling all ship to users.
We take security reports for any of those components seriously and welcome good-faith research.

## Reporting a vulnerability

**Do not open a public issue, pull request, discussion, or chat message for a suspected vulnerability.**

Preferred channel: use GitHub's private vulnerability reporting.

1. Open the repository's **Security** tab.
2. Choose **Report a vulnerability**.
3. Fill in the advisory draft with the details described below.

If private reporting is unavailable in your environment, contact the maintainers at `security@example.com` <!-- Replace with a monitored security mailbox. --> and mention that the report is a HuxerUI security report in the subject.

Please include:

- The affected component and version: the release tag, or the commit and branch for `main`.
- The platform and configuration: Windows, macOS, Linux, Web, Android, or iOS, the target architecture, and whether the SDK came from a release archive, a source checkout, or `mcpp`.
- A description of the issue, including the security property that is violated: confidentiality, integrity, availability, sandboxing, privilege boundary, or supply chain.
- Reproduction steps and the smallest proof of concept you can share: files, crafted payloads, and commands.
- The impact you believe is reachable, and the prerequisites for an attacker to reach it.
- Whether you have disclosed the issue anywhere else, and any planned publication date.
- An optional CWE identifier and CVSS assessment.

## What to expect

- We acknowledge a new report within 3 business days.
- We triage it, confirm applicability, and share an initial assessment within 10 business days. If we cannot reproduce the issue we will say so and explain what we tried.
- We aim to release a fix within 30 days for critical and high severity issues, and within 90 days for medium and low severity issues. Release timing depends on platform validation and coordinated disclosure for each affected backend.
- We coordinate disclosure with the reporter and publish a GitHub Security Advisory for confirmed vulnerabilities. Where appropriate we request a CVE through GitHub advisories.
- We credit reporters in the advisory unless you ask to remain anonymous.
- We practice coordinated disclosure. We ask that you do not publish technical details before a fix or advisory is available. If a fix is not ready close to 90 days from the report, we will agree on a disclosure date with you instead of leaving the issue open-ended.

HuxerUI is pre-1.0 software. A fix may ship as a new release rather than a backport, and older tags are not maintained. We always document the fixed version in the advisory.

## Supported versions

| Version | Supported |
| --- | --- |
| Latest release on the `main` branch | Yes |
| Older tagged releases | No |
| Pre-release and unreleased branches | Best effort |

Security fixes target the latest release and the `main` branch. If you cannot upgrade to a fixed version, tell us in the report so we can discuss a workaround.

## Scope

In scope:

- The public headers under `include/huxerui/` and the shared runtime under `src/`.
- Platform backends under `platform/`, including windowing, input, text, rendering, accessibility, files, HTTP, notifications, permissions, and system integration.
- The SDK CLI and project templates under `tools/huxerui_cli/`.
- Build-time tooling: `tools/resource_compiler` (`hrc`), `tools/codegen`, and generated resource handling.
- Installation and update paths: `install.sh`, `install.ps1`, the `huxerui` CLI updater, and generated packaging (including the Windows installer helpers).
- The application-facing contracts that the framework documents: capability-style file grants, resource trust boundaries, HTTP transport behavior, platform payload serialization, and web or platform bridges.
- Supply-chain issues in release artifacts and in the dependency fetching used by the SDK and library helpers.

Out of scope unless the report demonstrates a framework defect:

- Vulnerabilities in application code built with HuxerUI, or in third-party libraries used by an application. Report those upstream, but do tell us if the framework makes the issue unavoidable or silently unsafe.
- Vulnerabilities in `3dparty/` dependencies. Report them to the upstream project first; tell us if a HuxerUI API exposes users to the issue without a workaround.
- Vulnerabilities in operating-system, browser, GTK, UIKit, Android SDK, or toolchain components themselves.
- Findings that only demonstrate a missing hardening measure, documentation disagreement, or an unused code path, without a concrete impact.
- Denial of service that requires the attacker to already control the target application's configuration, build inputs, or user account, unless it crosses a documented trust boundary.
- Social engineering, phishing, physical access, or attacks that require an already-compromised host.
- Automated scanner output without a human explanation of the exploitable impact.

## Research guidelines and safe harbor

We will not pursue legal action for good-faith security research that follows this policy. Good faith means:

- You test only builds you own, or applications you are authorized to test.
- You do not access, modify, or exfiltrate other users' data.
- You do not degrade availability for anyone other than your own test environment.
- You use the smallest proof of concept that demonstrates the issue, and you stop once you have confirmed it.
- You do not publish details before we coordinate disclosure.
- You report through the private channel above rather than public issues.

If you are unsure whether an action is acceptable, contact us before performing it.

## Security-relevant contracts

Several subsystems document the trust boundaries we intend to enforce. Bypassing one of these boundaries is usually a security bug rather than a feature request:

- [Files and application storage](docs/design/files.md): capability-style file references, grant scoping, traversal constraints, and deletion protection for application directories.
- [Resources](docs/design/resources.md): the installed resource package trust model, package path rules, and the compiled payload format.
- [URI and local file URIs](docs/design/uri.md): parsing, encoding, and the local file URI conversion rules.
- [HTTP Client](docs/design/http.md): request validation, streaming, cancellation, redirects, and platform transport responsibilities.
- [Web](docs/design/web.md): browser bridge boundaries, session identity, and host integration.
- [Permissions](docs/design/permissions.md): authorization states and platform settings behavior.
- [Platform support](docs/guide/platforms.md): supported hosts and the capabilities each backend provides.

Reports that show a documented boundary can be crossed are the most valuable ones. Reports that suggest a boundary should be documented or tightened are also welcome, but mark them as hardening suggestions so we can triage them appropriately.

## Attribution

We list credited reporters in the published advisory. If your organization requires a different format for acknowledgment, include that in the report.

## Changes to this policy

We update this policy as the supported platforms, release process, and disclosure commitments change. The current version is the one on the `main` branch.
