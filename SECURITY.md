# Security Policy

HuxerUI is a cross-platform UI framework: application code, the shared runtime, platform backends, the SDK, and the build-time resource tooling all ship to users.
We take security reports for any of those components seriously and welcome good-faith research.

## Reporting a vulnerability

Submit suspected vulnerabilities using GitHub's private reporting form. [Report a vulnerability](https://github.com/HuxerUI/HuxerUI/security/advisories/new)

Keep report details private until disclosure is coordinated with the maintainers.

Include the following in your report.

- The affected component and version: the release tag, or the commit and branch for `main`.
- The platform and configuration: Windows, macOS, Linux, Web, Android, or iOS, the target architecture, and whether the SDK came from a release archive, a source checkout, or `mcpp`.
- A description of the issue, including the security property that is violated: confidentiality, integrity, availability, sandboxing, privilege boundary, or supply chain.
- Reproduction steps and the smallest proof of concept you can share: files, crafted payloads, and commands.
- The impact you believe is reachable, and the prerequisites for an attacker to reach it.
- Whether you have disclosed the issue anywhere else, and any planned publication date.
- An optional CWE identifier and CVSS assessment.

## What to expect

- We triage reports and use the private advisory to ask for details and share our assessment.
- We prioritize fixes by severity and impact. Fix timing depends on the issue and the affected platforms.
- We coordinate disclosure with the reporter and publish a GitHub Security Advisory for confirmed vulnerabilities. We request a CVE through GitHub when appropriate.
- We credit reporters unless they ask to remain anonymous. Include any preferred attribution in the report.

## Supported versions

| Version | Supported |
| --- | --- |
| Latest stable release | Yes |
| `main` | Yes |
| Older stable releases | No |
| Other branches and prereleases | Best effort |

Fixes may land on `main` and ship in a new release. The advisory identifies the fixed versions. If you cannot upgrade to a fixed version, mention it in the report so we can discuss a workaround.

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

## Research guidelines

- Test only builds you own or applications you are authorized to test.
- Keep tests isolated from other users' data and services.
- Use the smallest demonstration needed to confirm the issue and stop once it is confirmed.
- Report privately and coordinate disclosure with the maintainers.

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

## Changes to this policy

We update this policy as the supported platforms, release process, and disclosure commitments change. The current version is the one on the `main` branch.
