# Security Policy

## Supported Versions

The following versions of Scaleable-MPMC-Queue-cpp are currently supported with security updates:

| Version | Supported          |
| ------- | ------------------ |
| 1.0.x   | :white_check_mark: |
| < 1.0   | :x:                |

## Reporting a Vulnerability

We take security vulnerabilities seriously. If you discover a security issue, please report it through **GitHub Issues** with the `security` label.

### How to Report

1. Open a new issue at the project's GitHub Issues page
2. Add the `security` label to the issue
3. Include the following information:
   - **Description**: A clear description of the vulnerability
   - **Steps to Reproduce**: Detailed steps to reproduce the issue
   - **Impact Assessment**: Your assessment of the potential impact
   - **Affected Version(s)**: Which version(s) are affected
   - **Possible Fix**: If you have suggestions for fixing the issue (optional)

### Response Timeline

- **Initial Response**: Within 48 hours of report submission
- **Status Update**: Within 7 days with assessment and planned actions
- **Resolution**: Timeline depends on severity (see Security Update Policy below)

## Security Update Policy

Security vulnerabilities will be addressed based on their severity:

| Severity | Response Time | Action |
| -------- | ------------- | ------ |
| Critical | Immediate | Patch release within 24-48 hours |
| High | Within 7 days | Dedicated patch release |
| Medium | Next release | Included in next scheduled release |
| Low | Next release | Included in next scheduled release |

Severity is determined based on:
- Exploitability of the vulnerability
- Potential impact on users
- Scope of affected functionality

## Responsible Disclosure

We kindly request that you:

1. **Allow 90 days** before public disclosure to give us time to address the vulnerability
2. **Avoid exploiting** the vulnerability beyond what is necessary to demonstrate it
3. **Do not share** details of the vulnerability with others until it has been resolved

### Recognition

We appreciate the security research community's efforts in helping keep our project secure:

- Security researchers who report valid vulnerabilities will be credited in the release notes (unless they prefer to remain anonymous)
- We maintain a list of contributors who have helped improve our security

## Scope

This security policy applies to:
- The core MPMC queue implementation
- All public APIs and interfaces
- Build and test infrastructure

## Contact

For all security-related matters, please use **GitHub Issues** with the `security` label. This ensures proper tracking and timely response to security concerns.

---

Thank you for helping keep Scaleable-MPMC-Queue-cpp secure!
