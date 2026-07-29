# dev-sys-cloud 部署 Makefile
# 用法: make -f deploy/Makefile <target>

BINARY  := build/bin/dev-sys-cloud
VERSION := $(shell git describe --tags --always 2>/dev/null || echo "1.0.0")
ARCH    := $(shell uname -m)
PACKAGE := dev-sys-cloud-$(VERSION)-$(ARCH).tar.gz
DIST    := deploy/dist/$(VERSION)

.PHONY: all build test package deploy clean docker docker-package

all: build

build:
	cmake -B build -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTS=OFF -DUSE_SYSTEM_PAHO=OFF
	cmake --build build -j$$(nproc)
	strip $(BINARY)
	@echo "Binary: $(BINARY) ($$(ls -lh $(BINARY) | awk '{print $$5}'))"

package: build
	@echo "Packaging $(PACKAGE)..."
	rm -rf $(DIST) && mkdir -p $(DIST)
	cp $(BINARY) $(DIST)/
	cp config/mqtt_config.json $(DIST)/
	cp deploy/production/dev-sys-cloud.service $(DIST)/
	cp deploy/install.sh $(DIST)/
	chmod +x $(DIST)/install.sh
	cd $(DIST) && tar czf ../../$(PACKAGE) .
	@echo "Package: deploy/$(PACKAGE) ($$(ls -lh deploy/$(PACKAGE) | awk '{print $$5}'))"

deploy: package
	@if [ -z "$(SSH_HOST)" ]; then echo "Usage: make deploy SSH_HOST=user@host"; exit 1; fi
	scp deploy/$(PACKAGE) $(SSH_HOST):/tmp/
	ssh $(SSH_HOST) "cd /tmp && tar xzf $(PACKAGE) && sudo bash install.sh"

docker:
	docker build -f deploy/Dockerfile -t dev-sys-cloud:$(VERSION) .

docker-package: build
	rm -rf $(DIST)-docker && mkdir -p $(DIST)-docker
	cp $(BINARY) $(DIST)-docker/
	cp deploy/Dockerfile.runtime $(DIST)-docker/Dockerfile
	cp config/mqtt_config.json $(DIST)-docker/
	cd $(DIST)-docker && tar czf ../../dev-sys-cloud-docker-$(VERSION).tar.gz .
	@echo "Docker package: dev-sys-cloud-docker-$(VERSION).tar.gz"
	@echo "部署步骤:"
	@echo "  1. scp dev-sys-cloud-docker-$(VERSION).tar.gz ubuntu@39.106.70.145:/tmp/"
	@echo "  2. ssh ubuntu@39.106.70.145 && cd /tmp && tar xzf dev-sys-cloud-docker-$(VERSION).tar.gz"
	@echo "  3. docker build -t dev-sys-cloud ."
	@echo "  4. docker run -d --name dev-sys-cloud -p 9080:9080 -e DEV_SYS_DB='...' dev-sys-cloud"

clean:
	rm -rf build deploy/dist deploy/*.tar.gz
