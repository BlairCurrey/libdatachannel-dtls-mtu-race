FROM node:24-bookworm-slim
WORKDIR /app
COPY package.json package-lock.json repro.mjs ./
RUN npm ci --no-audit --no-fund
CMD ["node", "repro.mjs"]
