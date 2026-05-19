---
name: production-readiness-docs
description: Create comprehensive production readiness documentation package for software projects. Use this skill whenever the user mentions production readiness, deployment docs, operations manual, preparing for release, going to production, or needs deployment guide, monitoring setup, security checklist, or runbook creation. This skill generates the complete documentation ecosystem needed for production deployment including deployment guides, operations manuals, monitoring configuration, security checklists, CI/CD setup, and operational automation scripts.
compatibility:
  - Works with any software project (web apps, APIs, services)
  - Language-agnostic documentation templates
  - Requires understanding of project architecture and deployment requirements
---

# Production Readiness Documentation

Create a comprehensive documentation package that prepares your software project for production deployment.

## What This Skill Does

This skill generates the complete documentation ecosystem needed for production readiness:

### Core Documentation

1. **Deployment Guide** (`deployment_guide.md`)
   - System requirements (hardware, software)
   - Dependency installation
   - Configuration steps
   - Deployment procedures
   - Verification and troubleshooting

2. **Operations Manual** (`operations_manual.md`)
   - Daily operations (start, stop, restart)
   - Configuration updates
   - Log management
   - Troubleshooting procedures
   - Backup and recovery
   - Scaling strategies

3. **Monitoring Setup** (`monitoring_setup.md`)
   - Prometheus configuration
   - Grafana dashboards
   - Alerting rules
   - Metrics definitions
   - Log aggregation

4. **Security Checklist** (`security_checklist.md`)
   - API key management
   - Data protection
   - Authentication and authorization
   - Security headers
   - Dependency scanning

5. **Release Notes** (`release_notes_vX.X.md`)
   - New features
   - Performance metrics
   - Test coverage
   - Migration guide
   - Known issues

6. **CI/CD Configuration**
   - Build pipelines
   - Test automation
   - Deployment automation
   - Environment management

### Operational Scripts

7. **Automation Scripts**
   - Database backup/restore
   - Service restart
   - Log viewing
   - Health checks

## Documentation Structure

```
docs/
├── README.md (documentation index)
├── deployment_guide.md
├── operations_manual.md
├── monitoring_setup.md
├── security_checklist.md
├── release_notes_v1.0.md
└── archive/

deploy/
├── prometheus_config.yml
├── alerts.yml
├── grafana_dashboard.json
└── ops/
    ├── backup_db.sh
    ├── restore_db.sh
    ├── restart_service.sh
    └── view_logs.sh

.github/workflows/
├── ci.yml
└── deploy.yml
```

## Step-by-Step Creation Process

### 1. Gather Project Information

Before creating documentation, understand:

```bash
# Identify project type
ls -la package.json pom.xml build.gradle CMakeLists.txt

# Check current monitoring
ls -la config.json prometheus.yml

# Identify databases
grep -r "postgres\|mysql\|mongodb" .

# Check for existing docs
ls -la docs/ README.md
```

**Key questions to answer:**
- What type of application is this? (web service, API, batch job)
- What databases are used? (PostgreSQL, MySQL, MongoDB, Redis)
- What's the deployment model? (container, VM, bare metal, cloud)
- What's the technology stack? (language, framework, build tools)
- Are there existing CI/CD pipelines?

### 2. Create Deployment Guide

**Structure:**
```markdown
# Deployment Guide

## System Requirements
- Hardware (CPU, RAM, Disk)
- Software dependencies
- Network requirements

## Installation
### Dependency Installation
### Configuration
### Startup
```

**Customize based on project:**
- Add actual commands for the project
- Include specific version numbers
- Provide both Windows and Linux instructions
- Include environment variables

### 3. Create Operations Manual

**Structure:**
```markdown
# Operations Manual

## Daily Operations
### Start/Stop Services
### Configuration Updates
### Log Viewing

## Troubleshooting
### Common Issues
### Diagnostics
### Data Recovery

## Backup and Recovery
### Database Backups
### Configuration Backups
### Restore Procedures

## Scaling
### Horizontal Scaling
### Vertical Scaling
```

### 4. Create Monitoring Configuration

**Prometheus configuration:**
```yaml
global:
  scrape_interval: 15s

scrape_configs:
  - job_name: 'myapp'
    static_configs:
      - targets: ['localhost:8080']
    metrics_path: '/metrics'
```

**Alerting rules:**
```yaml
groups:
  - name: alerts
    rules:
      - alert: HighErrorRate
        expr: rate(errors[5m]) > 0.05
        for: 5m
```

### 5. Create Security Checklist

Organize by security domain:
- Authentication & Authorization
- Data Protection
- API Security
- Infrastructure Security
- Compliance & Auditing
- Secret Management

### 6. Create Release Notes Template

```markdown
# Release Notes v1.0.0

## Overview
## New Features
## Performance
## Testing
## Migration
## Known Issues
```

### 7. Create CI/CD Pipelines

**CI pipeline (.github/workflows/ci.yml):**
```yaml
name: CI
on: [push, pull_request]
jobs:
  build-and-test:
    runs-on: ubuntu-latest
    steps:
      - uses: actions/checkout@v3
      - name: Build
        run: |
          # Build commands
      - name: Test
        run: |
          # Test commands
```

**CD pipeline (.github/workflows/deploy.yml):**
```yaml
name: Deploy
on:
  push:
    branches: [main]
jobs:
  deploy:
    runs-on: ubuntu-latest
    steps:
      - name: Deploy
        run: |
          # Deployment commands
```

### 8. Create Operational Scripts

**Database backup script:**
```bash
#!/bin/bash
# Variables
DB_NAME="${DB_NAME:-mydb}"
BACKUP_DIR="/var/backups"

# Backup
pg_dump -U user "$DB_NAME" > "$BACKUP_DIR/backup.sql"
```

**Service restart script:**
```bash
#!/bin/bash
# Restart service
systemctl restart myapp
systemctl status myapp
```

## Customization Guidelines

### Web Applications

For web apps/APIs:
- Add API endpoint documentation
- Include load balancer configuration
- Document session management
- Add SSL/TLS setup

### Database-Heavy Applications

For apps with heavy database usage:
- Add migration procedures
- Include database backup/restore
- Document connection pooling
- Add query optimization tips

### Microservices

For microservice architectures:
- Add service discovery documentation
- Include inter-service communication
- Document distributed tracing
- Add service mesh configuration

## Best Practices

### DO:
- ✅ Be specific with actual commands and versions
- ✅ Include both Windows and Linux instructions when applicable
- ✅ Provide example configuration files
- ✅ Add troubleshooting sections
- ✅ Include verification steps
- ✅ Create operational scripts
- ✅ Document dependencies and versions

### DON'T:
- ❌ Use placeholders like "install your dependencies"
- ❌ Skip security considerations
- ❌ Forget rollback procedures
- ❌ Ignore monitoring and observability
- ❌ Leave out backup and recovery
- ❌ Make assumptions about user expertise

## Verification

After creating documentation:

1. **Review for completeness**
   - All sections filled in
   - No placeholder text
   - Commands tested for accuracy

2. **Test deployment instructions**
   - Follow the guide in a clean environment
   - Verify all steps work

3. **Review for clarity**
   - New user can follow along
   - Steps are in logical order
   - Troubleshooting covers common issues

4. **Validate operational scripts**
   - Scripts work as documented
   - Error handling is correct
   - Scripts are executable

## Templates

Use templates in `templates/` directory:
- `deployment_guide_template.md`
- `operations_manual_template.md`
- `monitoring_setup_template.md`
- `security_checklist_template.md`
- `release_notes_template.md`
- `ci_pipeline_template.yml`
- `cd_pipeline_template.yml`

## Example Outputs

Each document should be:
- **Comprehensive** - Cover all aspects
- **Practical** - Real commands, not theory
- **Actionable** - User can follow and succeed
- **Specific** - Actual versions, real commands
- **Verified** - Tested for accuracy

## Related Skills

- **cpp-project-file-cleanup** - Organize documentation before creating docs
- **monitoring-stack-setup** - Specific monitoring setup
- **ops-automation-scripts** - Create operational automation

## Success Criteria

Documentation package is complete when:
- ✅ All 6 core documents created
- ✅ Operational scripts created and tested
- ✅ CI/CD pipelines configured
- ✅ Deployment guide tested in clean environment
- ✅ Documentation index (docs/README.md) created
- ✅ Templates provided for future updates
