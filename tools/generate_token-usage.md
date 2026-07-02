# Generate token usage

1. Basic token (uses RDWS_JWT_SECRET from environment)
python3 tools/generate_token.py

1. With specific subject and roles
python3 tools/generate_token.py --subject joao@empresa.com --role admin

1. Ready to copy to your REST API client (with "Bearer ")
python3 tools/generate_token.py --subject dev --bearer

1. Valid for 1 year 
python3 tools/generate_token.py --ttl 31536000 --bearer

1. Using same vars from gateway environment
RDWS_JWT_SECRET=meu-segredo-dev RDWS_JWT_ISSUER=rdws-dev \
  python3 tools/generate_token.py --subject alice@empresa.com --role editor