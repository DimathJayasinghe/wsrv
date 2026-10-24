# Convenience wrapper around the Docker dev environment. The real build is CMake (see CMakePresets.json, M0).
COMPOSE := docker compose
RUN     := $(COMPOSE) exec dev

export PROJECT_DIR := $(CURDIR)
export USER_UID := $(shell id -u)
export USER_GID := $(shell id -g)

.PHONY: help image up down shell root-shell status clean-cache

help: ## Show targets
	@grep -E '^[a-z-]+:.*##' $(MAKEFILE_LIST) | awk -F':.*## ' '{printf "  %-12s %s\n", $$1, $$2}'

image: ## Build the dev image (host UID/GID baked in)
	$(COMPOSE) build dev

up: ## Start the dev container in the background
	$(COMPOSE) up -d dev

shell: up ## Open a shell as user 'dev' (passwordless sudo available)
	$(RUN) bash

root-shell: up ## Open a root shell in the container
	$(COMPOSE) exec -u root dev bash

status: ## Show toolchain versions inside the container
	$(RUN) bash -c 'g++ --version | head -1; clang++ --version | head -1; cmake --version | head -1; ninja --version; clang-format --version; clang-tidy --version | head -2; openssl version; php --version | head -1'

down: ## Stop the dev container
	$(COMPOSE) down

clean-cache: ## Remove the ccache volume
	$(COMPOSE) down -v
